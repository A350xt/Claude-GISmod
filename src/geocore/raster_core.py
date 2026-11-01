"""栅格基建：统一数据形状、模板对齐、统计摘要与 GeoTIFF 落盘。

三条约定（对标矢量域既有哲学）：
1. 模板显式化——所有合成/组合算子默认对齐首个输入的模板（origin/cellsize/
   行列数/CRS），不一致时自动重采样并写 warning；
2. 统计摘要随产物——每个栅格算子的 summaries 自带元信息 + nodata 占比 +
   分位数，Agent 可在下一步前自查（对标 preview/allow_empty 语义）；
3. nodata 统一用 NaN 表达（内部 float32），落盘写为 GeoTIFF 的 NaN nodata。
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from geocore.protocol import E_BAD_REQUEST, GeoCoreError, json_safe

RASTER_EXTS = {".tif", ".tiff"}
MAX_CELLS = 4_000_000  # 单幅栅格像元上限，防失控


@dataclass
class RasterFrame:
    """单波段栅格：float32 + NaN=nodata。transform 为 rasterio Affine。"""
    data: np.ndarray
    transform: object            # affine.Affine
    crs: object                  # pyproj.CRS | str | None
    name: str = "raster"

    # ---------------------------------------------------------------- helpers

    @property
    def shape(self) -> tuple:
        return self.data.shape

    @property
    def cellsize(self) -> float:
        return float(abs(self.transform.a))

    @property
    def bounds(self) -> tuple:
        from affine import Affine

        t = self.transform
        assert isinstance(t, Affine)
        h, w = self.data.shape
        minx, maxy = t * (0, 0)
        maxx, miny = t * (w, h)
        return (minx, miny, maxx, maxy)

    def is_empty(self) -> bool:
        return bool(np.all(np.isnan(self.data)))

    def meta(self) -> dict:
        """元信息 + nodata 占比 + 分位数（json_safe，可直接进响应）。"""
        v = self.data[np.isfinite(self.data)]
        q = np.quantile(v, [0, .05, .25, .5, .75, .95, 1]) if v.size else [float("nan")] * 7
        return {
            "shape": [int(self.data.shape[0]), int(self.data.shape[1])],
            "cellsize": round(self.cellsize, 6),
            "crs": str(self.crs) if self.crs is not None else None,
            "nodata_pct": round(float(np.isnan(self.data).mean() * 100), 2),
            "valid_cells": int(v.size),
            "quantiles": {p: round(float(x), 6) for p, x in
                          zip(["min", "p05", "p25", "p50", "p75", "p95", "max"], q)},
        }

    def summaries(self) -> list[str]:
        m = self.meta()
        q = m["quantiles"]
        return [
            f"栅格 {self.name}：{m['shape'][1]}×{m['shape'][0]} 像元，"
            f"cellsize={m['cellsize']}，CRS={m['crs']}",
            f"有效像元 {m['valid_cells']}（NoData {m['nodata_pct']}%）；"
            f"min={q['min']} p50={q['p50']} max={q['max']}",
        ]

    def table(self) -> dict:
        m = self.meta()
        return {
            "columns": ["metric", "value"],
            "rows": [[k, v] for k, v in m["quantiles"].items()]
            + [["nodata_pct", m["nodata_pct"]], ["valid_cells", m["valid_cells"]],
               ["cellsize", m["cellsize"]]],
        }


# ---------------------------------------------------------------- load / save

def is_raster_path(path_str: str) -> bool:
    return Path(str(path_str)).suffix.lower() in RASTER_EXTS


def load_raster(path_str: str, band: int = 1) -> RasterFrame:
    import rasterio

    path = Path(str(path_str))
    if not path.exists():
        raise GeoCoreError("E_INPUT_MISSING", f"栅格不存在：{path}", {"path": str(path)})
    try:
        with rasterio.open(path) as src:
            n = int(src.count)
            if band < 1 or band > n:
                raise GeoCoreError(E_BAD_REQUEST,
                                   f"波段号 {band} 超出范围（共 {n} 个波段）",
                                   {"bands": n})
            raw = src.read(band).astype("float32")
            nodata = src.nodata
            if nodata is not None:
                raw[raw == nodata] = np.nan
            # 统一转 pyproj CRS：rasterio CRS 缺 axis_info 等 API，会让
            # 投影单位判断静默失败（实测差一个 UTM 带号）
            from pyproj import CRS

            crs = CRS.from_user_input(src.crs) if src.crs is not None else None
            return RasterFrame(raw, src.transform, crs, name=path.stem)
    except GeoCoreError:
        raise
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"栅格读取失败：{exc}", {"path": str(path)}) from exc


def raster_meta_of(path_str: str) -> dict:
    """只读元信息（不进内存）：供 inspect 与清单展示。"""
    import rasterio

    path = Path(str(path_str))
    try:
        with rasterio.open(path) as src:
            v = src.read(1, masked=True)
            data = np.asarray(v, dtype="float32")
            finite = data[np.isfinite(data)]
            q = (np.quantile(finite, [0, .05, .25, .5, .75, .95, 1]).tolist()
                 if finite.size else [])
            return {
                "kind": "raster",
                "bands": int(src.count),
                "shape": [int(src.height), int(src.width)],
                "cellsize": [float(abs(src.transform.a)), float(abs(src.transform.e))],
                "crs": src.crs.to_string() if src.crs else None,
                "bounds": list(map(float, src.bounds)),
                "nodata": src.nodata,
                "dtype": src.dtypes[0],
                "nodata_pct": round(float(np.isnan(data).mean() * 100), 2) if finite.size or True else 0,
                "quantiles": {"min": q[0], "p50": q[3], "max": q[6]} if q else None,
            }
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"栅格元信息读取失败：{exc}", {"path": str(path)}) from exc


def save_raster(rf: RasterFrame, path: Path) -> Path:
    """float32 GeoTIFF，NaN 为 nodata。"""
    import rasterio

    _check_cells(rf)
    path.parent.mkdir(parents=True, exist_ok=True)
    h, w = rf.data.shape
    try:
        with rasterio.open(
            path, "w", driver="GTiff", height=h, width=w, count=1,
            dtype="float32", crs=rf.crs, transform=rf.transform, nodata=float("nan"),
        ) as dst:
            dst.write(rf.data.astype("float32"), 1)
        return path
    except Exception as exc:
        raise GeoCoreError("E_OUTPUT_ERROR", f"栅格写出失败：{exc}", {"path": str(path)}) from exc


def _check_cells(rf: RasterFrame) -> None:
    n = int(rf.data.size)
    if n == 0:
        raise GeoCoreError(E_BAD_REQUEST, "栅格为空（0 像元）")
    if n > MAX_CELLS:
        raise GeoCoreError(
            E_BAD_REQUEST,
            f"栅格过大：{n} 像元超过上限 {MAX_CELLS}；请增大 cellsize 或缩小范围",
            {"cells": n, "limit": MAX_CELLS},
        )


# ---------------------------------------------------------------- 对齐 / 重投影

def same_grid(a: RasterFrame, b: RasterFrame) -> bool:
    return (a.data.shape == b.data.shape
            and np.allclose([a.transform.c, a.transform.f], [b.transform.c, b.transform.f])
            and abs(a.transform.a - b.transform.a) < 1e-9
            and str(a.crs) == str(b.crs))


def reproject_rf(rf: RasterFrame, crs, transform=None, shape=None) -> RasterFrame:
    """重投影（可同时换网格）；transform/shape 缺省沿用源网格。"""
    import rasterio.warp
    from rasterio.warp import Resampling

    if transform is None:
        transform = rf.transform
    if shape is None:
        shape = rf.data.shape
    dst = np.full(shape, np.nan, dtype="float32")
    try:
        rasterio.warp.reproject(
            source=rf.data,
            destination=dst,
            src_transform=rf.transform,
            src_crs=rf.crs,
            dst_transform=transform,
            dst_crs=crs,
            src_nodata=float("nan"),
            dst_nodata=float("nan"),
            resampling=Resampling.nearest,
        )
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"栅格重投影失败：{exc}") from exc
    return RasterFrame(dst, transform, crs, name=rf.name)


def align_to(rf: RasterFrame, template: RasterFrame, method: str = "nearest") -> RasterFrame:
    """把 rf 对齐到 template 的网格与 CRS（不一致时才动作）。"""
    if same_grid(rf, template):
        return rf
    import rasterio.warp
    from rasterio.warp import Resampling

    resampling = {"nearest": Resampling.nearest, "bilinear": Resampling.bilinear,
                  "cubic": Resampling.cubic}.get(method)
    if resampling is None:
        raise GeoCoreError(E_BAD_REQUEST,
                           f"未知重采样方法：{method}（可选 nearest/bilinear/cubic）")
    dst = np.full(template.data.shape, np.nan, dtype="float32")
    try:
        rasterio.warp.reproject(
            source=rf.data, destination=dst,
            src_transform=rf.transform, src_crs=rf.crs,
            dst_transform=template.transform, dst_crs=template.crs,
            src_nodata=float("nan"), dst_nodata=float("nan"),
            resampling=resampling,
        )
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"栅格对齐失败：{exc}") from exc
    return RasterFrame(dst, template.transform, template.crs, name=rf.name)


def rasterize_vector(gdf, transform, shape, field=None, default=1.0, all_touched=False) -> np.ndarray:
    """矢量 → 布尔/数值栅格（field 给值列，否则 default）。"""
    import rasterio.features

    if field is not None:
        if field not in gdf.columns:
            raise GeoCoreError("E_INPUT_MISSING",
                               f"字段不存在：{field}",
                               {"columns": [str(c) for c in gdf.columns]})
        shapes = ((geom, float(val)) for geom, val in
                  zip(gdf.geometry, gdf[field]) if geom is not None and val == val)
    else:
        shapes = ((geom, float(default)) for geom in gdf.geometry if geom is not None)
    out = np.full(shape, np.nan, dtype="float32")
    try:
        rasterio.features.rasterize(
            shapes, out_shape=shape, transform=transform, fill=float("nan"),
            out=out, all_touched=all_touched, dtype="float32",
        )
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"栅格化失败：{exc}") from exc
    return out


def make_transform(bounds: tuple, cellsize: float, snap: bool = True):
    """bounds=(minx,miny,maxx,maxy) → 左上角原点的 Affine。"""
    from affine import Affine

    minx, miny, maxx, maxy = (float(b) for b in bounds)
    if maxx <= minx or maxy <= miny:
        raise GeoCoreError(E_BAD_REQUEST, f"bounds 非法：{bounds}")
    if cellsize <= 0:
        raise GeoCoreError(E_BAD_REQUEST, f"cellsize 必须为正数：{cellsize}")
    if snap:  # 网格对齐到 cellsize 整数倍，避免不同来源栅格错半像元
        minx = np.floor(minx / cellsize) * cellsize
        maxy = np.ceil(maxy / cellsize) * cellsize
    width = max(1, int(np.ceil((maxx - minx) / cellsize - 1e-9)))
    height = max(1, int(np.ceil((maxy - miny) / cellsize - 1e-9)))
    if width * height > MAX_CELLS:
        raise GeoCoreError(
            E_BAD_REQUEST,
            f"栅格模板过大：{height}×{width} 超过 {MAX_CELLS} 像元上限；"
            f"请增大 cellsize 或缩小范围",
            {"shape": [height, width]},
        )
    return Affine(cellsize, 0, minx, 0, -cellsize, maxy), (height, width)


def bounds_of_raster(rf: RasterFrame) -> tuple:
    return rf.bounds
