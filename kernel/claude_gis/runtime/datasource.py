"""Unified vector data loading for GeoJSON / GeoPackage / Shapefile / CSV+coords."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import geopandas as gpd
import pandas as pd

from claude_gis.protocol import E_BAD_REQUEST, E_CRS_AMBIGUOUS, E_EMPTY_RESULT, E_INPUT_MISSING, GisError

LON_CANDIDATES = ("lon", "lng", "longitude", "x")
LAT_CANDIDATES = ("lat", "latitude", "y")

VECTOR_EXTS = {".geojson", ".json", ".gpkg", ".shp"}


def _looks_like_lon(values: pd.Series) -> bool:
    v = pd.to_numeric(values, errors="coerce").dropna()
    return len(v) > 0 and v.between(-180, 180).all()


def _looks_like_lat(values: pd.Series) -> bool:
    v = pd.to_numeric(values, errors="coerce").dropna()
    return len(v) > 0 and v.between(-90, 90).all()


def _coerce_datetime_columns(df: pd.DataFrame) -> tuple[pd.DataFrame, list[str]]:
    """把 ISO 日期/时间文本列自动解析为 datetime64，让时间成为一等数据。

    只在「非空值 ≥80% 成功解析为时间且原本不是数值」时转换，避免误伤编码列。
    返回 (df, 转换的列名列表)。
    """
    converted: list[str] = []
    for col in df.columns:
        s = df[col]
        if pd.api.types.is_datetime64_any_dtype(s) or pd.api.types.is_numeric_dtype(s):
            continue
        nonnull = s.dropna()
        if len(nonnull) == 0:
            continue
        parsed = pd.to_datetime(nonnull, errors="coerce", format="ISO8601")
        if parsed.notna().mean() >= 0.8:
            df[col] = pd.to_datetime(s, errors="coerce", format="ISO8601")
            converted.append(str(col))
    return df, converted


def _load_csv_points(path: Path, layer: str | None) -> gpd.GeoDataFrame:
    df = pd.read_csv(path)
    df, _dt_cols = _coerce_datetime_columns(df)
    lon_col = next((c for c in df.columns if c.strip().lower() in LON_CANDIDATES), None)
    lat_col = next((c for c in df.columns if c.strip().lower() in LAT_CANDIDATES), None)
    if lon_col is None or lat_col is None:
        raise GisError(
            E_BAD_REQUEST,
            f"CSV 文件缺少经纬度列，无法识别为空间数据：{path}",
            {"expected_lon": list(LON_CANDIDATES), "expected_lat": list(LAT_CANDIDATES)},
        )
    if not (_looks_like_lon(df[lon_col]) and _looks_like_lat(df[lat_col])):
        raise GisError(
            E_CRS_AMBIGUOUS,
            f"CSV 的 {lon_col}/{lat_col} 列数值不在经纬度范围内，拒绝猜测坐标系",
            {"path": str(path)},
        )
    gdf = gpd.GeoDataFrame(
        df.drop(columns=[lon_col, lat_col]).copy(),
        geometry=gpd.points_from_xy(pd.to_numeric(df[lon_col]), pd.to_numeric(df[lat_col])),
        crs="EPSG:4326",
    )
    return gdf


def load_dataset(path_str: str, layer: str | None = None) -> tuple[gpd.GeoDataFrame, dict[str, Any]]:
    """Load any supported vector source.

    Returns (GeoDataFrame, provenance dict). Raises GisError with a stable code.
    """
    path = Path(path_str)
    if not path.exists():
        raise GisError(E_INPUT_MISSING, f"输入数据不存在：{path}", {"path": str(path)})

    ext = path.suffix.lower()

    if ext == ".csv":
        gdf = _load_csv_points(path, layer)
        prov = {"format": "csv+lonlat", "path": str(path), "crs": "EPSG:4326"}
        return gdf, prov

    if ext not in VECTOR_EXTS:
        raise GisError(
            E_BAD_REQUEST,
            f"不支持的文件格式：{ext}（支持 GeoJSON/GeoPackage/Shapefile/CSV+经纬度）",
            {"path": str(path)},
        )

    try:
        if ext == ".gpkg":
            layers = gpd.list_layers(path)
            names = layers["name"].tolist()
            target_layer = layer
            if target_layer is None:
                if len(names) == 0:
                    raise GisError(E_BAD_REQUEST, f"GeoPackage 中没有图层：{path}", {"path": str(path)})
                target_layer = names[0]
            elif target_layer not in names:
                raise GisError(
                    E_INPUT_MISSING,
                    f"图层不存在：{target_layer}",
                    {"path": str(path), "available_layers": names},
                )
            gdf = gpd.read_file(path, layer=target_layer, engine="pyogrio")
            prov = {"format": "gpkg", "path": str(path), "layer": target_layer}
        else:
            if layer:
                raise GisError(E_BAD_REQUEST, f"{ext} 格式不支持 layer 参数", {"path": str(path)})
            gdf = gpd.read_file(path, engine="pyogrio")
            fmt = "geojson" if ext in (".geojson", ".json") else "shapefile"
            prov = {"format": fmt, "path": str(path)}

        if len(gdf) == 0:
            raise GisError(E_EMPTY_RESULT, f"数据集为空（0 条要素）：{path}", {"path": str(path)})
        if gdf.crs is None and ext == ".geojson":
            # RFC 7946: GeoJSON 默认 WGS84
            gdf = gdf.set_crs("EPSG:4326")
        return gdf, prov
    except GisError:
        raise
    except Exception as exc:  # unreadable/corrupt file etc.
        raise GisError(E_BAD_REQUEST, f"读取失败:{exc}", {"path": str(path), "raw_error": repr(exc)}) from exc


def read_json_file(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)
