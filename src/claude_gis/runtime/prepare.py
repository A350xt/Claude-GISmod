"""Automatic data preparation: geometry repair and CRS normalization.

Reliability rules (never silent):
- invalid geometries are repaired via make_valid and reported as warnings;
- a missing CRS may only be assumed as EPSG:4326 when coordinates are in range,
  otherwise the request fails with E_CRS_AMBIGUOUS;
- any measurement op runs in a projected metric CRS, chosen once per request.
"""

from __future__ import annotations

import geopandas as gpd
from shapely.validation import explain_validity

from claude_gis.protocol import E_CRS_AMBIGUOUS, GisError


def repair_geometries(gdf: gpd.GeoDataFrame, warnings: list[str]) -> gpd.GeoDataFrame:
    """Fix invalid geometries; drop empty ones. All repairs are reported."""
    if gdf.empty or "geometry" not in gdf.columns:
        return gdf

    valid_mask = gdf.geometry.notna() & ~gdf.geometry.is_empty
    n_null = int((~valid_mask).sum())
    if n_null > 0:
        warnings.append(f"{n_null} 条要素无几何或几何为空，已剔除")
        gdf = gdf[valid_mask].copy()

    if gdf.empty:
        return gdf

    invalid_mask = ~gdf.geometry.is_valid
    n_invalid = int(invalid_mask.sum())
    if n_invalid == 0:
        return gdf

    reasons = {explain_validity(geom) for geom in gdf.loc[invalid_mask, "geometry"].head(5)}
    repaired = gdf.copy()
    fixed = repaired.loc[invalid_mask, "geometry"].make_valid()
    # make_valid 可能产生 GeometryCollection，取最大面积部分作为代表几何
    from shapely.geometry.base import BaseGeometry, BaseMultipartGeometry
    from shapely.geometry import GeometryCollection

    def _simplify_collection(geom):
        if isinstance(geom, GeometryCollection):
            parts = [g for g in geom.geoms if not g.is_empty]
            polys = [g for g in parts if g.geom_type in ("Polygon", "MultiPolygon")]
            lines = [g for g in parts if g.geom_type in ("LineString", "MultiLineString")]
            points = [g for g in parts if g.geom_type in ("Point", "MultiPoint")]
            for group in (polys, lines, points):
                if group:
                    return group[0] if len(group) == 1 else _unary(group)
        return geom

    def _unary(parts):
        from shapely.ops import unary_union

        return unary_union(parts)

    repaired.loc[invalid_mask, "geometry"] = [
        _simplify_collection(g) if isinstance(g, BaseGeometry) else g for g in fixed
    ]
    still_bad = ~repaired.geometry.is_valid | repaired.geometry.is_empty
    if bool(still_bad.any()):
        raise GisError(
            E_GEOMETRY_INVALID_UNFIXABLE,
            f"{int(still_bad.sum())} 条要素几何无法修复",
            {"samples": list(reasons)},
        )
    warnings.append(f"自动修复了 {n_invalid} 个无效几何（{'；'.join(sorted(reasons))}）")
    return repaired


def assume_missing_crs(gdf: gpd.GeoDataFrame, provenance: str, warnings: list[str]) -> gpd.GeoDataFrame:
    """Only ever assume EPSG:4326, and only when coordinates are safely in range."""
    if gdf.crs is not None:
        return gdf
    minx, miny, maxx, maxy = gdf.total_bounds
    in_range = -181 <= minx <= 181 and -91 <= miny <= 91 and -181 <= maxx <= 181 and -91 <= maxy <= 91
    if not in_range:
        raise GisError(
            E_CRS_AMBIGUOUS,
            f"数据缺失 CRS 且坐标值不在经纬度范围内，无法安全假定坐标系：{provenance}",
            {"bounds": [float(minx), float(miny), float(maxx), float(maxy)]},
        )
    warnings.append(f"{provenance} 缺失 CRS：坐标在经纬度范围内，已按 EPSG:4326 处理")
    return gdf.set_crs("EPSG:4326")


def estimate_utm(gdf: gpd.GeoDataFrame):
    """geopandas 的 estimate_utm_crs 封装：按数据范围选择 UTM 带，失败时报领域错误。"""
    try:
        target = gdf.estimate_utm_crs()
    except Exception as exc:
        raise GisError(
            E_CRS_AMBIGUOUS,
            "无法为该地理范围确定合适的米制投影坐标系",
            {"error": repr(exc)},
        ) from exc
    if target is None:
        raise GisError(E_CRS_AMBIGUOUS, "无法为该地理范围确定合适的米制投影坐标系")
    return target


class MetricCRSPlan:
    """One projected CRS per analysis request; every metric op reuses it."""

    def __init__(self, warnings: list[str]):
        self.warnings = warnings
        self.target_crs = None
        self.sources: dict[str, str] = {}

    def to_metric(self, gdf: gpd.GeoDataFrame, name: str) -> tuple[gpd.GeoDataFrame, dict]:
        """Return (gdf in metric crs, info dict describing the decision)."""
        src_crs = gdf.crs
        self.sources[name] = str(src_crs.to_string() if src_crs is not None else "")
        if self.target_crs is None:
            if src_crs is None:
                raise GisError(E_CRS_AMBIGUOUS, f"{name}: 无 CRS，禁止度量运算")
            if src_crs.is_geographic:
                target = estimate_utm(gdf)
            else:
                axis_unit = _axis_unit(src_crs)
                if axis_unit and "metre" not in axis_unit.lower():
                    self.warnings.append(
                        f"{name} 使用非米制投影（{axis_unit}），度量结果按坐标单位给出"
                    )
                target = src_crs
            self.target_crs = target
        out = gdf.to_crs(self.target_crs) if gdf.crs != self.target_crs else gdf
        changed = str(gdf.crs) != str(out.crs)
        if changed:
            self.warnings.append(f"{name} 已从 {src_crs.to_string()} 重投影到 {self.target_crs.to_string()} 进行度量")
        info = {"source": src_crs.to_string() if src_crs is not None else "", "analysis": self.target_crs.to_string()}
        return out, info


def _axis_unit(crs) -> str | None:
    try:
        axes = crs.axis_info
        return axes[0].unit_name if axes else None
    except Exception:
        return None
