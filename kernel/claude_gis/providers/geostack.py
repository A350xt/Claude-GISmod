from __future__ import annotations

"""Geopandas/shapely implementation of the Provider interface."""

import re

import geopandas as gpd

from claude_gis.protocol import E_BAD_REQUEST, E_EMPTY_RESULT, E_INPUT_MISSING, GisError

_SAFE_QUERY_TOKENS = re.compile(r"\b(import|exec|eval|open|__|getattr|setattr|system)\b")

_BACKTICKED = re.compile(r"`([^`\n]{1,64})`")


def _with_backticked_aliases(gdf: gpd.GeoDataFrame, where: str):
    """``class`==…` 这类关键字/特殊列名用反引号书写；这里改写成安全别名再执行查询。"""
    rename: dict[str, str] = {}
    taken = set(gdf.columns)

    def _alias(match: re.Match) -> str:
        original = match.group(1)
        candidate = "_qc0"
        n = 0
        while candidate in taken:
            n += 1
            candidate = f"_qc{n}"
        taken.add(candidate)
        rename[original] = candidate
        return candidate

    translated = _BACKTICKED.sub(_alias, where)
    return gdf.rename(columns={orig: alias for orig, alias in rename.items()}), translated

SPATIAL_PREDICATES = ("intersects", "within", "contains", "touches", "crosses", "overlaps")

_POINTY = {"Point", "MultiPoint"}
_LINEY = {"LineString", "MultiLineString"}
_POLYGONY = {"Polygon", "MultiPolygon"}


def _require_nonempty(*gdfs, label: str) -> None:
    for pos, gdf in enumerate(gdfs):
        if gdf is None or len(gdf) == 0:
            raise GisError(
                E_EMPTY_RESULT,
                f"{label}: 输入为空，无法继续运算（第 {pos + 1} 个输入）",
                {"position": pos},
            )


def _geom_family(types: set[str]) -> str:
    if types <= _POINTY:
        return "point"
    if types <= _LINEY:
        return "line"
    if types <= _POLYGONY:
        return "polygon"
    raise GisError(
        E_BAD_REQUEST,
        f"几何类型混杂（{sorted(types)}），Phase 1 单步运算要求同族几何",
        {"types": sorted(types)},
    )


class GeoStackProvider:
    """纯 Python 地理栈后端。输入必须是已对齐 CRS 的有效几何。"""

    # ------------------------------------------------------- Query & Measure

    def filter_attributes(self, gdf: gpd.GeoDataFrame, where: str) -> gpd.GeoDataFrame:
        if not isinstance(where, str) or not where.strip():
            raise GisError(E_BAD_REQUEST, "filter 需要非空 where 表达式")
        if _SAFE_QUERY_TOKENS.search(where.lower()):
            raise GisError(E_BAD_REQUEST, "where 含不允许的语法", {"where": where})
        frame, translated = _with_backticked_aliases(gdf, where)
        try:
            return frame.query(translated, engine="python").copy()
        except Exception as exc:
            raise GisError(
                E_BAD_REQUEST,
                f"where 表达式无效:{exc}",
                {"where": where, "grammar": "pandas query 语法；`反引号` 包裹的列名会自动改写"},
            ) from exc

    def select_spatial(self, gdf: gpd.GeoDataFrame, predicate: str, ref: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        if predicate not in SPATIAL_PREDICATES:
            raise GisError(E_BAD_REQUEST, f"未知空间谓词:{predicate}", {"allowed": list(SPATIAL_PREDICATES)})
        ref_union = ref.union_all()
        mask = getattr(gdf.geometry, predicate)(ref_union)
        return gdf[mask.fillna(False)].copy()

    def measure(self, gdf: gpd.GeoDataFrame, measures: list[str]) -> tuple[gpd.GeoDataFrame, dict]:
        allowed = {"area_sqkm", "area_ha", "length_km", "count"}
        bad = set(measures) - allowed
        if bad:
            raise GisError(E_BAD_REQUEST, f"未知度量:{sorted(bad)}", {"allowed": sorted(allowed)})
        fam = _geom_family(set(map(str, gdf.geometry.geom_type)))
        out = gdf.copy()
        totals: dict[str, float] = {}
        for m in measures:
            if m == "count":
                totals["count"] = float(len(out))
                continue
            if m.startswith("area"):
                if fam != "polygon":
                    raise GisError(E_BAD_REQUEST, "面积度量仅适用于面要素")
                sqkm = out.geometry.area / 1e6
                col = m if m not in out.columns else m + "_calc"
                out[col] = sqkm
                totals[m + "_sum"] = float(sqkm.sum())
            else:
                if fam != "line":
                    raise GisError(E_BAD_REQUEST, "长度度量仅适用于线要素")
                km = out.geometry.length / 1000.0
                col = m if m not in out.columns else m + "_calc"
                out[col] = km
                totals[m + "_sum"] = float(km.sum())
        return out, totals

    # -------------------------------------------------------------- Proximity

    def buffer(self, gdf: gpd.GeoDataFrame, distance_m: float, dissolve: bool, quad_segs: int) -> gpd.GeoDataFrame:
        buffered = gdf.copy()
        buffered["geometry"] = gdf.geometry.buffer(float(distance_m), resolution=max(4, int(quad_segs)))
        if dissolve:
            merged = buffered.union_all()
            single = gpd.GeoDataFrame({"distance_m": [float(distance_m)]},
                                      geometry=[merged], crs=gdf.crs)
            return single
        buffered["buffer_distance_m"] = float(distance_m)
        return buffered

    def within_distance(self, gdf: gpd.GeoDataFrame, ref: gpd.GeoDataFrame, distance_m: float) -> gpd.GeoDataFrame:
        ref_union = ref.union_all()
        distances = gdf.geometry.distance(ref_union)
        mask = distances <= float(distance_m)
        out = gdf[mask].copy()
        out["_distance_m"] = distances[mask]
        return out.sort_values("_distance_m").drop(columns=["_distance_m"])

    def nearest(self, gdf: gpd.GeoDataFrame, ref: gpd.GeoDataFrame, k: int, join_attrs: list[str]) -> gpd.GeoDataFrame:
        """精确 k 近邻：投影坐标上的向量化距离矩阵（O(n·m)，超过配额明确拒绝）。

        选择手写 KNN 而非 sjoin_nearest：后者无 k 参数且同名列会被自动改写后缀。
        """
        import numpy as np
        import shapely

        n_left, n_right = len(gdf), len(ref)
        if n_left == 0 or n_right == 0:
            raise GisError(
                E_EMPTY_RESULT,
                "nearest：输入或参照层为空，无法计算最近邻",
                {"n_left": n_left, "n_right": n_right},
            )
        if n_left * n_right > 25_000_000:
            raise GisError(
                E_BAD_REQUEST,
                f"最近邻规模过大：{n_left}×{n_right}；请先缩小输入范围",
                {"n_left": n_left, "n_right": n_right},
            )
        left = gdf.reset_index(drop=True).copy()
        right = ref.reset_index(drop=True)
        kk = max(1, min(int(k), n_right))
        dist_matrix = shapely.distance(
            np.asarray(left.geometry.values)[:, None],
            np.asarray(right.geometry.values)[None, :],
        )
        order = np.argsort(dist_matrix, axis=1)[:, :kk]

        attrs = [a for a in (join_attrs or []) if a in right.columns]
        rows = []
        for li in range(n_left):
            for rank, ri in enumerate(order[li], start=1):
                row = left.iloc[li].to_dict()
                row["nearest_rank"] = rank
                row["nearest_distance_m"] = float(dist_matrix[li, ri])
                src = right.iloc[ri]
                for attr in attrs:
                    row[f"nearest_{attr}"] = src[attr]
                rows.append(row)

        out = gpd.GeoDataFrame(rows, geometry="geometry", crs=left.crs)
        return out

    # ---------------------------------------------------------------- Overlay

    @staticmethod
    def _normalize_suffixes(out: gpd.GeoDataFrame,
                            cols_a: set[str], cols_b: set[str]) -> gpd.GeoDataFrame:
        """gpd.overlay 对两输入的同名列加数字后缀(_1/_2)；归一为语义化 _a/_b。"""
        renames = {}
        for col in out.columns:
            base, _, num = str(col).rpartition("_")
            if base and num in ("1", "2") and base in cols_a and base in cols_b:
                renames[col] = f"{base}_a" if num == "1" else f"{base}_b"
        return out.rename(columns=renames)

    def intersection(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        out = gpd.overlay(a, b, how="intersection", keep_geom_type=True)
        return self._normalize_suffixes(out, set(map(str, a.columns)), set(map(str, b.columns)))

    def difference(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        return gpd.overlay(a, b, how="difference", keep_geom_type=True)

    def union(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        out = gpd.overlay(a, b, how="union", keep_geom_type=False)
        return self._normalize_suffixes(out, set(map(str, a.columns)), set(map(str, b.columns)))

    def clip(self, a: gpd.GeoDataFrame, mask: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        mask_only = gpd.GeoDataFrame(geometry=mask.geometry, crs=mask.crs)
        return gpd.overlay(a, mask_only, how="intersection", keep_geom_type=True)

    # ------------------------------------------------------------------ Zonal

    def zonal_summarize(
        self,
        regions: gpd.GeoDataFrame,
        data: gpd.GeoDataFrame,
        stats: list[str],
        field: str | None,
    ) -> tuple[gpd.GeoDataFrame, dict]:
        _require_nonempty(regions, data, label="zonal.summarize")
        fam = _geom_family({str(t) for t in data.geometry.geom_type})
        wanted = {s for s in (stats or ["count"]) }
        known = {"count", "sum", "mean", "min", "max", "total_length_km", "total_area_sqkm", "share_pct"}
        unknown = wanted - known
        if unknown:
            raise GisError(E_BAD_REQUEST, f"未知统计量:{sorted(unknown)}", {"known": sorted(known)})
        if ({"sum", "mean"} & wanted) and not field:
            raise GisError(E_BAD_REQUEST, "sum/mean 统计需要提供 field 字段名")
        if field and field not in data.columns:
            raise GisError(E_INPUT_MISSING, f"字段不存在:{field}", {"columns": list(map(str, data.columns))})

        if fam == "point":
            return self._zonal_points(regions, data, wanted, field)
        if fam == "line":
            return self._zonal_lines(regions, data, wanted, field)
        return self._zonal_polygons(regions, data, wanted, field)

    # zonal internals ---------------------------------------------------------

    def _region_join(self, regions: gpd.GeoDataFrame, data: gpd.GeoDataFrame) -> gpd.GeoDataFrame:
        left = data.reset_index(drop=True).copy()
        left["_didx"] = range(len(left))
        right = regions.reset_index().rename(columns={"index": "_ridx"})[["_ridx", "geometry"]]
        parts = gpd.overlay(left, right, how="intersection", keep_geom_type=True)
        return parts

    def _zonal_points(self, regions, data, wanted, field):
        parts = self._region_join(regions, data)
        out = regions.reset_index(drop=True).copy()
        counts = parts.groupby("_ridx").size()
        out["data_count"] = out.index.map(counts).fillna(0).astype("int64")
        if field and {"sum", "mean", "min", "max"} & wanted:
            agg = parts.groupby("_ridx")[field]
            if "sum" in wanted:
                out[f"data_{field}_sum"] = out.index.map(agg.sum()).fillna(0)
            if "mean" in wanted:
                out[f"data_{field}_mean"] = out.index.map(agg.mean())
            if "min" in wanted:
                out[f"data_{field}_min"] = out.index.map(agg.min())
            if "max" in wanted:
                out[f"data_{field}_max"] = out.index.map(agg.max())
        info = {"kind": "point", "grand_total": int(len(data)), "matched": int(len(parts))}
        return out.set_geometry("geometry"), info

    def _zonal_lines(self, regions, data, wanted, field):
        parts = self._region_join(regions, data)
        full_len = data.geometry.length
        parts["_full_len"] = parts["_didx"].map(full_len)
        parts["_frac"] = parts.geometry.length / parts["_full_len"].replace(0, 1)
        out = regions.reset_index(drop=True).copy()
        counts = parts.groupby("_ridx")["_didx"].nunique()
        lengths_m = parts.groupby("_ridx").apply(lambda g: (g.geometry.length).sum(), include_groups=False)
        total_len_m = float(full_len.sum())
        out["data_count"] = out.index.map(counts).fillna(0).astype("int64")
        if "total_length_km" in wanted or "share_pct" in wanted:
            km = out.index.map(lengths_m).fillna(0) / 1000.0
            out["data_length_km"] = km
            if "share_pct" in wanted:
                out["data_length_share_pct"] = (km * 1000 / max(total_len_m, 1e-9)) * 100.0
        if field and "sum" in wanted:
            contrib = parts.assign(_c=parts[field] * parts["_frac"]).groupby("_ridx")["_c"].sum()
            out[f"data_{field}_sum_apportioned"] = out.index.map(contrib).fillna(0)
        info = {"kind": "line", "grand_total_length_km": round(total_len_m / 1000, 6)}
        return out.set_geometry("geometry"), info

    def _zonal_polygons(self, regions, data, wanted, field):
        parts = self._region_join(regions, data)
        full_area = data.geometry.area
        parts["_full_area"] = parts["_didx"].map(full_area)
        parts["_frac"] = parts.geometry.area / parts["_full_area"].replace(0, 1)
        out = regions.reset_index(drop=True).copy()
        counts = parts.groupby("_ridx")["_didx"].nunique()
        areas_m2 = parts.groupby("_ridx").apply(lambda g: (g.geometry.area).sum(), include_groups=False)
        out["data_count"] = out.index.map(counts).fillna(0).astype("int64")
        if "total_area_sqkm" in wanted or "share_pct" in wanted:
            sqkm = out.index.map(areas_m2).fillna(0) / 1e6
            out["data_area_sqkm"] = sqkm
            if "share_pct" in wanted:
                total = float(full_area.sum())
                out["data_area_share_pct"] = (sqkm * 1e6 / max(total, 1e-9)) * 100.0
        if field and "sum" in wanted:
            contrib = parts.assign(_c=parts[field] * parts["_frac"]).groupby("_ridx")["_c"].sum()
            out[f"data_{field}_sum_apportioned"] = out.index.map(contrib).fillna(0)
        if field and "mean" in wanted:
            num = parts.assign(_w=parts[field] * parts["_frac"])
            wsum = parts.groupby("_ridx")["_frac"].sum()
            wavg = num.groupby("_ridx").apply(lambda g: g["_w"].sum() / max(g["_frac"].sum(), 1e-12),
                                              include_groups=False)
            out[f"data_{field}_area_weighted_mean"] = out.index.map(wavg)
        info = {"kind": "polygon", "grand_total_area_sqkm": round(float(full_area.sum()) / 1e6, 6)}
        return out.set_geometry("geometry"), info


PROVIDER = GeoStackProvider()
