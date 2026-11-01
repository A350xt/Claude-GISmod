"""栅格算子（raster 域）：镜像矢量域的小算子哲学——一算子一语义、@步骤链式、
统计摘要随产物、模板显式化（组合时自动对齐到首个输入并写 warning）。

词汇总览（详见 docs/tool-api.md）：
  基建    raster.info / create / from_vector / align / mask / merge
  距离    raster.distance / distance_decay / cost_distance / cost_path / nearest
  逐像元  raster.con / calc / reclassify / breaks / histogram / weighted_sum
  邻域    raster.focal / zonal_stats
  转出    raster.polygonize / contour / sample
"""

from __future__ import annotations

import ast
import heapq
import math
import operator as _op

import numpy as np

from claude_gis.analysis.common import StepResult, need, opt
from claude_gis.protocol import (
    E_BAD_REQUEST,
    E_CRS_AMBIGUOUS,
    E_EMPTY_RESULT,
    E_INPUT_MISSING,
    GisError,
)
from claude_gis.raster_core import (
    MAX_CELLS,
    RasterFrame,
    _check_cells,
    align_to,
    make_transform,
    rasterize_vector,
    same_grid,
)

_OPS = {
    ast.Add: _op.add, ast.Sub: _op.sub, ast.Mult: _op.mul,
    ast.Div: _op.truediv, ast.Pow: _op.pow, ast.Mod: _op.mod,
}
_FUNCS = {
    "abs": np.abs, "min": np.minimum, "max": np.maximum,
    "round": np.round, "clip": np.clip, "sqrt": np.sqrt, "exp": np.exp, "ln": np.log,
}
_DECAY_MODES = ("linear", "square", "exponential")


def _raster_param(resolver, token: str, label: str) -> RasterFrame:
    rf, _ = resolver.raster(str(token))
    return rf


def _vector_param(resolver, token: str, label: str):
    gdf, _ = resolver.frame(str(token))
    if len(gdf) == 0:
        raise GisError(E_EMPTY_RESULT, f"{label}：输入为空", {})
    return gdf


def _done(rf: RasterFrame, summaries: list[str], table: dict | None = None) -> StepResult:
    _check_cells(rf)
    return StepResult(None, summaries + rf.summaries(), table=table, raster=rf)


def _warn_aligned(resolver, name: str, rf: RasterFrame, template: RasterFrame) -> RasterFrame:
    if same_grid(rf, template):
        return rf
    resolver.warnings.append(
        f"{name} 与首输入模板不一致（origin/cellsize/形状），已自动重采样对齐"
        f"（{rf.data.shape}→{template.data.shape}, cellsize {rf.cellsize}→{template.cellsize}）"
    )
    return align_to(rf, template)


# ==================================================================== 基建

def handle_info(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.info"), "raster.info")
    return StepResult(None, rf.summaries() + [
        "元信息见 steps[].table；如需直方图/断点，接 raster.histogram / raster.breaks",
    ], table=rf.table(), raster=rf)


def handle_create(resolver, params, step_id):
    raw_bounds = params.get("bounds")
    if (not isinstance(raw_bounds, (list, tuple)) or len(raw_bounds) != 4):
        raise GisError(E_BAD_REQUEST,
                           "bounds 需要 [minx, miny, maxx, maxy]（分析坐标系的米制范围）")
    cellsize = float(need(params, "cellsize", "raster.create"))
    value = params.get("value")
    crs = str(params.get("crs") or "") or None
    if crs is None:
        if resolver._analysis_crs is not None:
            crs = resolver._analysis_crs
        else:
            raise GisError(E_CRS_AMBIGUOUS,
                               "create 需要 crs（或先在管线里引用一个输入以确定坐标系）")
    transform, shape = make_transform(raw_bounds, cellsize)
    if shape[0] * shape[1] > MAX_CELLS:
        raise GisError(E_BAD_REQUEST,
                           f"范围×cellsize 过大（{shape[0]}×{shape[1]}）；请增大 cellsize 或缩小 bounds")
    data = (np.full(shape, float(value), dtype="float32")
            if value is not None else np.full(shape, np.nan, dtype="float32"))
    rf = RasterFrame(data, transform, crs, name=str(params.get("name") or "created"))
    return _done(rf, [f"创建空模板栅格：{shape[1]}×{shape[0]}，cellsize={cellsize}，"
                      f"初值={'NoData' if value is None else value}"])


def handle_from_vector(resolver, params, step_id):
    gdf = _vector_param(resolver, need(params, "input", "raster.from_vector"), "from_vector")
    field = params.get("field")
    template_tok = params.get("template")
    if template_tok:
        tmpl = _raster_param(resolver, template_tok, "from_vector.template")
        transform, shape, crs = tmpl.transform, tmpl.data.shape, tmpl.crs
        note = f"对齐模板 {template_tok}"
    else:
        cellsize = float(need(params, "cellsize", "raster.from_vector"))
        bounds = gdf.total_bounds.tolist()
        transform, shape = make_transform(bounds, cellsize)
        crs = gdf.crs
        note = f"cellsize={cellsize}（矢量外接矩形）"
    if shape[0] * shape[1] > MAX_CELLS:
        raise GisError(E_BAD_REQUEST,
                           f"栅格化结果过大（{shape[0]}×{shape[1]}）；请增大 cellsize")
    data = rasterize_vector(gdf, transform, shape, field=field,
                            all_touched=bool(params.get("all_touched", False)))
    if np.all(np.isnan(data)):
        raise GisError(E_EMPTY_RESULT,
                           "栅格化结果全空（矢量可能都在模板范围外）；如属预期请加 allow_empty")
    rf = RasterFrame(data, transform, crs, name=f"{field or 'mask'}")
    return _done(rf, [f"矢量栅格化：{len(gdf)} 要素 → {shape[1]}×{shape[0]} 像元，{note}，"
                      + (f"取字段 {field} 的值" if field else "覆盖处=1")])


def handle_align(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.align"), "align")
    tmpl = _raster_param(resolver, need(params, "template", "raster.align"), "align.template")
    method = str(opt(params, "method", "nearest"))
    out = _warn_aligned(resolver, "align.input", rf, tmpl)
    return _done(RasterFrame(out.data, tmpl.transform, tmpl.crs, name=rf.name),
                 [f"对齐到模板（method={method}）"])


def handle_mask(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.mask"), "mask")
    invert = bool(params.get("invert", False))
    fill = params.get("fill")  # None → NoData；数值 → 填充值
    mask_tok = need(params, "mask", "raster.mask")
    if str(mask_tok).lower().endswith((".tif", ".tiff")) or str(mask_tok).startswith("@") \
            or str(mask_tok).startswith("ar_"):
        mrf = _raster_param(resolver, mask_tok, "mask.raster")
        mrf = _warn_aligned(resolver, "mask", mrf, rf)
        keep = np.isfinite(mrf.data)
    else:
        gdf = _vector_param(resolver, mask_tok, "mask")
        vals = rasterize_vector(gdf, rf.transform, rf.data.shape)
        keep = np.isfinite(vals)
    if invert:
        keep = ~keep
    out = rf.data.copy()
    if fill is None:
        out[~keep] = np.nan
    else:
        out[~keep] = float(fill)
    n_kept = int(keep.sum())
    if n_kept == 0:
        raise GisError(E_EMPTY_RESULT,
                           "掩膜后全空；如属预期请给该步加 allow_empty")
    rf2 = RasterFrame(out, rf.transform, rf.crs, name=rf.name)
    return _done(rf2, [f"掩膜：保留 {n_kept} / {rf.data.size} 像元"
                       + ("（反转）" if invert else "")
                       + (f"，掩膜外填 {fill}" if fill is not None else "，掩膜外置 NoData")])


def handle_merge(resolver, params, step_id):
    toks = params.get("inputs")
    if not isinstance(toks, list) or len(toks) < 1:
        raise GisError(E_BAD_REQUEST, "merge 需要 inputs（栅格引用数组）")
    rfs = [_raster_param(resolver, t, "merge") for t in toks]
    base = rfs[0]
    # 联合范围（贴到 base 网格）
    minx = min(rf.bounds[0] for rf in rfs)
    miny = min(rf.bounds[1] for rf in rfs)
    maxx = max(rf.bounds[2] for rf in rfs)
    maxy = max(rf.bounds[3] for rf in rfs)
    transform, shape = make_transform((minx, miny, maxx, maxy), base.cellsize)
    canvas = np.full(shape, np.nan, dtype="float32")
    placed = 0
    for rf in rfs:
        aligned = align_to(rf, RasterFrame(canvas, transform, base.crs), method="nearest")
        m = np.isfinite(aligned.data) & np.isnan(canvas)
        canvas[m] = aligned.data[m]
        placed += int(m.sum())
    if placed == 0:
        raise GisError(E_EMPTY_RESULT, "镶嵌结果全空")
    rf2 = RasterFrame(canvas, transform, base.crs, name=base.name)
    return _done(rf2, [f"镶嵌 {len(rfs)} 幅 → {shape[1]}×{shape[0]}，有效像元 {placed}"
                       f"（重叠处先到先得）"])


# ==================================================================== 距离

def _distance_field(resolver, gdf, rf_template: RasterFrame, max_distance_m=None):
    from scipy import ndimage

    mask = np.isfinite(rasterize_vector(gdf, rf_template.transform, rf_template.data.shape))
    if not mask.any():
        raise GisError(E_EMPTY_RESULT, "源要素栅格化为空（可能在模板范围外）")
    dist = ndimage.distance_transform_edt(~mask,
                                          sampling=(rf_template.cellsize, rf_template.cellsize))
    out = dist.astype("float32")
    out[~np.isfinite(out)] = np.nan
    if max_distance_m is not None:
        out[out > float(max_distance_m)] = np.nan
    return out


def _padded_bounds(gdf, cellsize: float, pad_m: float = 0.0) -> tuple:
    """矢量外接矩形 + 外扩：保证退化范围（同一 y/x 的点列）也能建模板。"""
    b = gdf.total_bounds
    pad = max(float(pad_m), 2.0 * cellsize)
    return (float(b[0]) - pad, float(b[1]) - pad,
            float(b[2]) + pad, float(b[3]) + pad)


def handle_distance(resolver, params, step_id):
    gdf = _vector_param(resolver, need(params, "input", "raster.distance"), "distance")
    cellsize = opt(params, "cellsize", None)
    if cellsize is None:
        b = gdf.total_bounds
        span = max(b[2] - b[0], b[3] - b[1]) or 1000.0
        cellsize = max(span / 400.0, 1.0)
    transform, shape = make_transform(
        _padded_bounds(gdf, float(cellsize), float(opt(params, "pad_m", 0))),
        float(cellsize))
    rf = RasterFrame(np.full(shape, np.nan, "float32"), transform, gdf.crs, name="dist")
    data = _distance_field(resolver, gdf, rf, params.get("max_distance_m"))
    rf2 = RasterFrame(data, transform, gdf.crs, name="distance_m")
    return _done(rf2, [f"欧氏距离场：源 {len(gdf)} 要素（源处=0），单位米"
                       + (f"，>{params['max_distance_m']}m 截断为 NoData"
                          if params.get("max_distance_m") is not None else "")])


def handle_distance_decay(resolver, params, step_id):
    d0 = float(need(params, "d0", "raster.distance_decay"))
    if d0 <= 0:
        raise GisError(E_BAD_REQUEST, f"d0 必须为正数：{d0}")
    f0 = float(opt(params, "f0", 100.0))
    mode = str(opt(params, "mode", "linear"))
    if mode not in _DECAY_MODES:
        raise GisError(E_BAD_REQUEST, f"未知 mode：{mode}（可选 {_DECAY_MODES}）")
    gdf = _vector_param(resolver, need(params, "input", "raster.distance_decay"), "decay")
    cellsize = opt(params, "cellsize", None)
    if cellsize is None:
        span = max(gdf.total_bounds[2] - gdf.total_bounds[0],
                   gdf.total_bounds[3] - gdf.total_bounds[1]) or d0
        cellsize = max(span / 400, d0 / 100, 1.0)
    transform, shape = make_transform(_padded_bounds(gdf, float(cellsize), d0 * 0.05),
                                      float(cellsize))
    rf = RasterFrame(np.full(shape, np.nan, "float32"), transform, gdf.crs)
    dist = _distance_field(resolver, gdf, rf)
    x = dist / d0
    x = np.clip(x, 0, 1)
    if mode == "linear":
        score = f0 * (1 - x)
    elif mode == "square":
        score = f0 * (1 - x * x)
    else:
        score = f0 * np.exp(-x)
    score[dist > d0] = 0.0
    rf2 = RasterFrame(score.astype("float32"), transform, gdf.crs, name="score")
    return _done(rf2, [f"距离衰减作用分：d0={d0}m, f0={f0}, mode={mode}；"
                       f"源处={f0}，>{d0}m=0"])


def _cost_dijkstra(cost: np.ndarray, sources: np.ndarray, cellsize: float):
    """Dijkstra 累积成本：返回 (dist, backlink)。cost 为 NaN 处不可通行。"""
    h, w = cost.shape
    INF = np.inf
    dist = np.full((h, w), INF, dtype="float64")
    back = np.full((h, w), -1, dtype="int32")
    walkable = np.isfinite(cost) & (cost > 0)
    pq = []
    for r, c in zip(*np.nonzero(sources)):
        if walkable[r, c]:
            dist[r, c] = 0.0
            heapq.heappush(pq, (0.0, int(r), int(c)))
    if not pq:
        raise GisError(E_EMPTY_RESULT, "成本距离：没有落在可通行区的源像元")
    nb = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))
    while pq:
        d, r, c = heapq.heappop(pq)
        if d > dist[r, c]:
            continue
        for dr, dc in nb:
            rr, cc = r + dr, c + dc
            if 0 <= rr < h and 0 <= cc < w and walkable[rr, cc]:
                step = cost[rr, cc] * cellsize * (math.sqrt(2) if dr and dc else 1.0)
                nd = d + step
                if nd < dist[rr, cc] - 1e-9:
                    dist[rr, cc] = nd
                    back[rr, cc] = r * w + c
                    heapq.heappush(pq, (nd, rr, cc))
    dist[~np.isfinite(dist)] = np.nan
    return dist, back


def handle_cost_distance(resolver, params, step_id):
    cost = _raster_param(resolver, need(params, "cost", "raster.cost_distance"), "cost")
    src_tok = need(params, "source", "raster.cost_distance")
    src = _source_mask(resolver, src_tok, cost)
    dist, _back = _cost_dijkstra(np.asarray(cost.data, dtype="float64"), src, cost.cellsize)
    if np.all(np.isnan(dist)):
        raise GisError(E_EMPTY_RESULT, "成本距离全空（源与成本面不连通？）")
    rf = RasterFrame(dist.astype("float32"), cost.transform, cost.crs, name="cost_dist")
    return _done(rf, [f"累积成本距离：单位 = cost值×米；NoData/≤0 成本视为不可通行"])


def handle_cost_path(resolver, params, step_id):
    cost = _raster_param(resolver, need(params, "cost", "raster.cost_path"), "cost_path")
    src_tok = need(params, "source", "raster.cost_path")
    to_tok = need(params, "to", "raster.cost_path")
    src = _source_mask(resolver, src_tok, cost)
    to_gdf = _vector_param(resolver, to_tok, "cost_path.to")
    if len(to_gdf) > 1:
        to_gdf = to_gdf.iloc[[0]]
        resolver.warnings.append("cost_path.to 有多个点，取第一个")
    dist, back = _cost_dijkstra(np.asarray(cost.data, dtype="float64"), src, cost.cellsize)
    import geopandas as gpd
    from shapely.geometry import LineString

    to_geom = to_gdf.geometry.iloc[0]
    col_f, row_f = ~cost.transform * (to_geom.x, to_geom.y)
    r, c = int(round(row_f)), int(round(col_f))
    h, w = cost.shape
    if not (0 <= r < h and 0 <= c < w):
        raise GisError(E_BAD_REQUEST, "目标点在成本栅格范围外")
    if not np.isfinite(dist[r, c]):
        raise GisError(E_EMPTY_RESULT,
                           "目标不可达（与源不连通或被 NoData/零成本阻断）")
    cells = [(r, c)]
    while back[r, c] != -1:
        r, c = divmod(int(back[r, c]), w)
        cells.append((r, c))
    coords = [cost.transform * (c + 0.5, r + 0.5) for r, c in cells]
    line = LineString(coords)
    out = gpd.GeoDataFrame(
        {"total_cost": [float(dist[cells[0][0], cells[0][1]])],
         "n_cells": [len(cells)]},
        geometry=[line], crs=cost.crs)
    return StepResult(out, [f"成本最短路径：{len(cells)} 像元，总成本 "
                            f"{float(dist[cells[0][0], cells[0][1]]):.3f}"])


def _source_mask(resolver, tok: str, template: RasterFrame) -> np.ndarray:
    tok = str(tok)
    if tok.startswith("@") or tok.startswith("ar_") or tok.lower().endswith((".tif", ".tiff")):
        try:
            rf = _raster_param(resolver, tok, "source")
        except GisError:
            gdf = _vector_param(resolver, tok, "source")
        else:
            rf = _warn_aligned(resolver, "source", rf, template)
            return np.isfinite(rf.data)
        gdf = _vector_param(resolver, tok, "source")
    else:
        gdf = _vector_param(resolver, tok, "source")
    return np.isfinite(rasterize_vector(gdf, template.transform, template.data.shape))


def handle_nearest(resolver, params, step_id):
    from scipy import ndimage

    gdf = _vector_param(resolver, need(params, "input", "raster.nearest"), "nearest")
    ref = _vector_param(resolver, need(params, "ref", "raster.nearest"), "nearest.ref")
    cellsize = opt(params, "cellsize", None)
    if cellsize is None:
        span = max(gdf.total_bounds[2] - gdf.total_bounds[0],
                   gdf.total_bounds[3] - gdf.total_bounds[1]) or 1000.0
        cellsize = span / 300
    transform, shape = make_transform(_padded_bounds(gdf, float(cellsize)), float(cellsize))
    # 每个参照要素一个 id（1..n），栅格化为"最近源分配"初值
    ids = rasterize_vector(ref.assign(_nid=range(1, len(ref) + 1)),
                           transform, shape, field="_nid")
    source_mask = np.isfinite(ids)
    if not source_mask.any():
        raise GisError(E_EMPTY_RESULT, "参照层栅格化为空")
    _, (ir, ic) = ndimage.distance_transform_edt(
        ~source_mask, return_indices=True,
        sampling=(float(cellsize), float(cellsize)))
    alloc = ids[ir, ic]
    # 距离场（供摘要；主产物是分配栅格）
    dist = ndimage.distance_transform_edt(
        ~source_mask, sampling=(float(cellsize), float(cellsize)))
    rf = RasterFrame(alloc.astype("float32"), transform, gdf.crs, name="nearest_id")
    finite = dist[np.isfinite(alloc)]
    return _done(rf, [
        f"就近分配：每个像元 = 最近参照要素的序号（1..{len(ref)}），"
        f"最大最近距离 {float(finite.max()):.1f} m；"
        f"距离场请用 raster.distance {{input, ref: 同参照层}}"
    ])


# ==================================================================== 逐像元

def handle_con(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.con"), "con")
    cond = _parse_condition(str(need(params, "condition", "raster.con")))
    data = rf.data
    is_nodata_cond = cond_is_nodata(cond)
    if is_nodata_cond:
        mask = np.isnan(data)
    else:
        with np.errstate(invalid="ignore"):
            mask = cond(data)
        # 比较条件对 NoData 不生效：NaN 像元保持 NoData（处理 NoData 请用 "nodata" 条件）
        mask = mask & np.isfinite(data)
    t = params.get("true")
    f = params.get("false")
    out = data.copy()

    def _val(v, where_mask):
        if isinstance(v, str) and v.strip().lower() == "nodata":
            out[where_mask] = np.nan
        elif v is None:
            pass  # 保留原值
        else:
            out[where_mask] = float(v)

    false_mask = ~mask if is_nodata_cond else (~mask & np.isfinite(data))
    _val(t, mask)
    _val(f, false_mask)
    rf2 = RasterFrame(out, rf.transform, rf.crs, name=rf.name)
    n_true = int(mask.sum())
    return _done(rf2, [f"条件赋值：{n_true} / {rf.data.size} 像元为真 → true={t!r}，false={f!r}"
                       "（比较条件不改动 NoData 像元；处理 NoData 用 condition='nodata'）"])


_CMP = {"<=": _op.le, ">=": _op.ge, "==": _op.eq, "!=": _op.ne,
        "<": _op.lt, ">": _op.gt}

_NODATA_COND = object()


def cond_is_nodata(cond) -> bool:
    return cond is _NODATA_COND


def _parse_condition(expr: str):
    """受限条件表达式：`值 <op> 阈值` 或 `nodata`；返回 ndarray→bool 的函数。"""
    e = expr.strip()
    if e.lower() in ("nodata", "isnan"):
        return _NODATA_COND
    for sym, fn in _CMP.items():
        if sym in e:
            left, _, right = e.partition(sym)
            right = right.strip()
            try:
                thr = float(right)
            except ValueError:
                raise GisError(
                    E_BAD_REQUEST,
                    f"condition 右侧需为数字：{expr!r}",
                    {"grammar": "值 <op> 数字，如 'value >= 500' 或 'nodata'；"
                                "左标识符随意（指代像元值）"})
            def make(fn, thr):
                return lambda arr: fn(arr, thr)
            return make(fn, thr)
    raise GisError(E_BAD_REQUEST,
                       f"无法解析 condition：{expr!r}",
                       {"grammar": "'value >= 500' / 'dist < d0' / 'nodata'"})


def handle_calc(resolver, params, step_id):
    expr = str(need(params, "expression", "raster.calc"))
    inputs = params.get("inputs")
    if not isinstance(inputs, dict) or not inputs:
        raise GisError(E_BAD_REQUEST,
                           "calc 需要 inputs：{别名: 栅格引用}，表达式中用别名引用")
    if len(inputs) > 8:
        raise GisError(E_BAD_REQUEST, "calc 最多 8 个输入栅格")
    rfs = {str(alias): _raster_param(resolver, tok, "calc")
           for alias, tok in inputs.items()}
    first = next(iter(rfs.values()))
    aligned = {}
    for alias, rf in rfs.items():
        aligned[alias] = _warn_aligned(resolver, f"calc.{alias}", rf, first).data
    tree = ast.parse(expr, mode="eval")

    def ev(node, names):
        if isinstance(node, ast.Expression):
            return ev(node.body, names)
        if isinstance(node, ast.Constant):
            if isinstance(node.value, (int, float)):
                return node.value
            raise GisError(E_BAD_REQUEST, f"不允许的常量：{node.value!r}")
        if isinstance(node, ast.Name):
            if node.id not in names:
                raise GisError(E_BAD_REQUEST,
                                   f"表达式中引用了未声明的别名：{node.id}",
                                   {"declared": sorted(names)})
            return names[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
            return _OPS[type(node.op)](ev(node.left, names), ev(node.right, names))
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            v = ev(node.operand, names)
            return v if isinstance(node.op, ast.UAdd) else -v
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) \
                and node.func.id in _FUNCS and len(node.args) in (1, 2, 3) \
                and not node.keywords:
            args = [ev(a, names) for a in node.args]
            return _FUNCS[node.func.id](*args)
        raise GisError(
            E_BAD_REQUEST,
            f"表达式含不允许的语法：{expr!r}",
            {"allowed": "四则/乘方/取模 + abs/min/max/round/clip/sqrt/exp/ln(别名…)"},
        )

    with np.errstate(invalid="ignore", divide="ignore"):
        out = ev(tree, aligned)
    out = np.asarray(out, dtype="float32")
    out[~np.isfinite(out)] = np.nan
    rf2 = RasterFrame(out, first.transform, first.crs, name="calc")
    return _done(rf2, [f"逐像元计算：{expr}（输入 {sorted(rfs)}）"])


def handle_reclassify(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.reclassify"), "reclassify")
    mapping = params.get("mapping")
    if (not isinstance(mapping, list) or not mapping
            or not all(isinstance(m, (list, tuple)) and len(m) == 3 for m in mapping)):
        raise GisError(
            E_BAD_REQUEST,
            "mapping 需要 [[lo, hi, new], …] 行表（左闭右开）",
            {"example": "[[0, 200, 1], [200, 400, 2]]"},
        )
    out = np.full(rf.data.shape, np.nan, dtype="float32")
    v = rf.data
    n_mapped = 0
    for lo, hi, new in mapping:
        m = (v >= float(lo)) & (v < float(hi))
        out[m] = float(new)
        n_mapped += int(m.sum())
    if params.get("nodata") is not None:
        out[np.isnan(v)] = float(params["nodata"])
    if n_mapped == 0:
        raise GisError(E_EMPTY_RESULT,
                           "重分类没有任何像元落表；检查区间与数值范围（可用 raster.breaks 先看断点）")
    rf2 = RasterFrame(out, rf.transform, rf.crs, name="class")
    classes = sorted({float(m[2]) for m in mapping})
    return _done(rf2, [f"重分类：{len(mapping)} 条规则，命中 {n_mapped} 像元，"
                       f"级别 {classes}；未落表且未给 nodata 的像元 = NoData"])


def handle_breaks(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.breaks"), "breaks")
    k = int(opt(params, "classes", 5))
    if not 2 <= k <= 20:
        raise GisError(E_BAD_REQUEST, f"classes 需在 2..20：{k}")
    method = str(opt(params, "method", "quantile"))
    v = rf.data[np.isfinite(rf.data)]
    if v.size < k:
        raise GisError(E_EMPTY_RESULT, f"有效像元 {v.size} 少于 classes {k}")
    if method == "equal_interval":
        breaks = np.linspace(float(v.min()), float(v.max()), k + 1).tolist()
    elif method == "quantile":
        breaks = np.quantile(v, np.linspace(0, 1, k + 1)).tolist()
    elif method == "jenks":
        breaks = _jenks(v, k)
    else:
        raise GisError(E_BAD_REQUEST,
                           f"未知 method：{method}（可选 equal_interval/quantile/jenks）")
    breaks = sorted(set(round(float(b), 6) for b in breaks))
    table = {"columns": ["idx", "lo", "hi"], "rows": [
        [i, breaks[i], breaks[i + 1]] for i in range(len(breaks) - 1)]}
    summaries = [
        f"断点（{method}，{len(breaks) - 1} 类）：只求断点不执行；"
        f"下一步接 raster.reclassify mapping=[[lo,hi,new]…]",
    ]
    return _done(rf, summaries, table=table)


def _jenks(values: np.ndarray, k: int) -> list:
    """Fisher-Jenks 自然断点（值抽样去重 ≤ 2000 控制 DP 规模）。"""
    v = np.unique(values)
    if v.size > 2000:
        idx = np.linspace(0, v.size - 1, 2000).astype(int)
        v = v[idx]
    n = v.size
    k = min(k, n - 1)
    mat1 = np.zeros((n + 1, k + 1), dtype=int)
    mat2 = np.full((n + 1, k + 1), np.inf)
    for j in range(1, k + 1):
        mat1[1, j] = 1
        mat2[1, j] = 0.0
    for l in range(2, n + 1):
        s1 = 0.0
        s2 = 0.0
        w = 0.0
        for m in range(1, l + 1):
            i3 = l - m + 1
            val = float(v[i3 - 1])
            s2 += val * val
            s1 += val
            w += 1
            if w == 1:
                continue
            sd = s2 - s1 * s1 / w
            for j in range(2, k + 1):
                if mat2[l, j] >= sd + mat2[i3 - 1, j - 1]:
                    mat2[l, j] = sd + mat2[i3 - 1, j - 1]
                    mat1[l, j] = i3 - 1
            if mat2[l, 1] > s2 - s1 * s1 / w:
                mat2[l, 1] = s2 - s1 * s1 / w
                mat1[l, 1] = l
    kclass = [0.0] * (k + 1)
    kclass[k] = float(v[n - 1])
    kclass[0] = float(v[0])
    l = n
    for j in range(k, 1, -1):
        idx = int(mat1[l, j]) - 2
        if idx < 0:
            break
        kclass[j - 1] = float(v[idx])
        l = int(mat1[l, j]) - 1
        if l < 2:
            break
    return kclass


def handle_histogram(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.histogram"), "histogram")
    bins = int(opt(params, "bins", 20))
    if not 2 <= bins <= 200:
        raise GisError(E_BAD_REQUEST, f"bins 需在 2..200：{bins}")
    v = rf.data[np.isfinite(rf.data)]
    counts, edges = np.histogram(v, bins=bins)
    total = int(counts.sum())
    cum = 0
    rows = []
    for i, c in enumerate(counts):
        cum += int(c)
        rows.append([round(float(edges[i]), 6), round(float(edges[i + 1]), 6),
                     int(c), round(cum * 100.0 / total, 2)])
    table = {"columns": ["lo", "hi", "count", "cumulative_pct"], "rows": rows}
    return _done(rf, [f"直方图：{bins} 桶，有效像元 {total}（累计频率可直接用于频率曲线定级）"],
                 table=table)


def handle_weighted_sum(resolver, params, step_id):
    items = params.get("inputs")
    if (not isinstance(items, list) or len(items) < 1
            or not all(isinstance(x, dict) and "ref" in x for x in items)):
        raise GisError(E_BAD_REQUEST,
                           "inputs 需要 [{ref, weight}, …]（weight 可省略=1）")
    weights = [float(x.get("weight", 1.0)) for x in items]
    if any(w < 0 for w in weights):
        raise GisError(E_BAD_REQUEST, "weight 不能为负")
    total_w = sum(weights)
    if total_w <= 0:
        raise GisError(E_BAD_REQUEST, "weight 全为 0，无法归一化")
    first = _raster_param(resolver, items[0]["ref"], "weighted_sum")
    acc = np.zeros(first.data.shape, dtype="float64")
    hit = np.zeros(first.data.shape, dtype=bool)
    for item, w in zip(items, weights):
        rf = _raster_param(resolver, item["ref"], "weighted_sum")
        data = _warn_aligned(resolver, f"weighted_sum.{item['ref']}", rf, first).data
        m = np.isfinite(data)
        acc[m] += data[m] * w
        hit |= m
    out = np.full(first.data.shape, np.nan, dtype="float32")
    out[hit] = (acc[hit] / total_w).astype("float32")
    rf2 = RasterFrame(out, first.transform, first.crs, name="weighted")
    return _done(rf2, [f"加权叠加：{len(items)} 层，权重 {[round(w, 4) for w in weights]} "
                       f"（自动归一化，和={round(total_w, 4)}）；某像元任一层 NoData 则不计该层"])


# ==================================================================== 邻域

def handle_focal(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.focal"), "focal")
    stat = str(opt(params, "stat", "mean")).lower()
    known = {"mean", "min", "max", "median", "std"}
    if stat not in known:
        raise GisError(E_BAD_REQUEST, f"未知 stat：{stat}（可选 {sorted(known)}）")
    k = int(opt(params, "kernel_size", 3))
    if k < 3 or k % 2 == 0 or k > 15:
        raise GisError(E_BAD_REQUEST, f"kernel_size 需为 3..15 的奇数：{k}")
    pad = k // 2
    padded = np.pad(rf.data, pad, constant_values=np.nan)
    from numpy.lib.stride_tricks import sliding_window_view

    win = sliding_window_view(padded, (k, k))
    with np.errstate(invalid="ignore"):
        if stat == "mean":
            out = np.nanmean(win, axis=(-2, -1))
        elif stat == "min":
            out = np.nanmin(win, axis=(-2, -1))
        elif stat == "max":
            out = np.nanmax(win, axis=(-2, -1))
        elif stat == "median":
            out = np.nanmedian(win, axis=(-2, -1))
        else:
            out = np.nanstd(win, axis=(-2, -1))
    out = np.asarray(out, dtype="float32")
    out[~np.isfinite(out)] = np.nan
    rf2 = RasterFrame(out, rf.transform, rf.crs, name=f"focal_{stat}")
    return _done(rf2, [f"邻域统计：{k}×{k} {stat}（边界像元按可用邻居计算）"])


def handle_zonal_stats(resolver, params, step_id):
    regions = _vector_param(resolver, need(params, "regions", "raster.zonal_stats"), "zonal")
    rf = _raster_param(resolver, need(params, "data", "raster.zonal_stats"), "zonal.data")
    if not set(map(str, regions.geometry.geom_type)) <= {"Polygon", "MultiPolygon"}:
        raise GisError(E_BAD_REQUEST, "regions 必须是面要素")
    stats = [str(s) for s in opt(params, "stats", ["mean", "min", "max"])]
    known = {"count", "mean", "min", "max", "sum", "majority"}
    unknown = set(stats) - known
    if unknown:
        raise GisError(E_BAD_REQUEST, f"未知统计量：{sorted(unknown)}", {"known": sorted(known)})
    from rasterio.features import geometry_mask

    out = regions.reset_index(drop=True).copy()
    rows_meta = []
    for i, geom in enumerate(out.geometry):
        m = ~geometry_mask([geom], out_shape=rf.data.shape, transform=rf.transform,
                           invert=False, all_touched=True)
        vals = rf.data[m & np.isfinite(rf.data)]
        if vals.size == 0:
            rows_meta.append(0)
            for s in stats:
                out.loc[i, f"raster_{s}"] = np.nan
            continue
        rows_meta.append(int(vals.size))
        for s in stats:
            if s == "count":
                out.loc[i, "raster_count"] = int(vals.size)
            elif s == "mean":
                out.loc[i, "raster_mean"] = float(np.mean(vals))
            elif s == "min":
                out.loc[i, "raster_min"] = float(np.min(vals))
            elif s == "max":
                out.loc[i, "raster_max"] = float(np.max(vals))
            elif s == "sum":
                out.loc[i, "raster_sum"] = float(np.sum(vals))
            else:  # majority
                u, c = np.unique(np.round(vals, 6), return_counts=True)
                out.loc[i, "raster_majority"] = float(u[np.argmax(c)])
    covered = sum(1 for n in rows_meta if n > 0)
    summaries = [
        f"栅格分区统计：{len(out)} 个区域，{covered} 个含有效像元"
        + (f"（{len(out) - covered} 个区域与栅格无有效交集）" if covered < len(out) else ""),
        f"统计量 {stats}，输出列前缀 raster_（数值列为各区域值）",
    ]
    return StepResult(out, summaries)


# ==================================================================== 转出（栅格→矢量闭环）

def handle_polygonize(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.polygonize"), "polygonize")
    from rasterio.features import shapes
    from shapely.geometry import shape as shp_shape
    import geopandas as gpd

    data = rf.data.copy()
    data[np.isnan(data)] = -9999.0  # shapes 不吃 NaN：用哨兵再剔除
    geoms, values = [], []
    for geom, val in shapes(data, mask=data != -9999.0, transform=rf.transform):
        geoms.append(shp_shape(geom))
        values.append(float(val))
    if not geoms:
        raise GisError(E_EMPTY_RESULT, "没有可转换的有效像元")
    out = gpd.GeoDataFrame({"value": values}, geometry=geoms, crs=rf.crs)
    field_name = str(opt(params, "field", "value"))
    out = out.rename(columns={"value": field_name})
    # 同值溶解（可选，默认开——级别区出图通常要连通同值面）
    if params.get("dissolve", True):
        out = out.dissolve(by=field_name, as_index=False)
    return StepResult(out, [
        f"栅格转面：{len(out)} 个多边形"
        + ("（同值溶解）" if params.get("dissolve", True) else "（逐同值区块）"),
        f"值字段 {field_name}；后续可直接接 query.filter / overlay / visualize",
    ])


def handle_contour(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "input", "raster.contour"), "contour")
    levels = params.get("levels")
    interval = params.get("interval")
    if levels is None and interval is None:
        v = rf.data[np.isfinite(rf.data)]
        if v.size == 0:
            raise GisError(E_EMPTY_RESULT, "无有效像元")
        span = float(v.max() - v.min())
        if span == 0:
            raise GisError(E_EMPTY_RESULT, "数值无变化，提不出等值线")
        interval = span / 8.0
    if levels is None:
        v = rf.data[np.isfinite(rf.data)]
        lo, hi = float(v.min()), float(v.max())
        levels = [round(float(x), 6) for x in
                  np.arange(lo + float(interval), hi, float(interval))]
        if not levels:
            raise GisError(E_EMPTY_RESULT,
                               f"interval={interval} 内没有任何等值线级别")
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import geopandas as gpd
    from shapely.geometry import LineString

    h, w = rf.data.shape
    x = np.arange(w) + 0.5
    y = np.arange(h) + 0.5
    fig = plt.figure()
    try:
        ax = fig.add_subplot(111)
        try:
            cs = ax.contour(x, y, rf.data, levels=levels)
        except ValueError as exc:
            raise GisError(E_BAD_REQUEST, f"等值线提取失败（检查 levels）：{exc}") from exc
        geoms, values = [], []
        from matplotlib.path import Path as MplPath

        for path, level in zip(cs.get_paths(), cs.levels):
            verts = np.asarray(path.vertices, dtype="float64")
            codes = (np.asarray(path.codes) if path.codes is not None else None)
            # 连续折线按 MOVETO/NaN 断点切段，每段一条 LineString
            brk = np.zeros(len(verts), dtype=bool)
            if len(verts) > 1:
                brk[1:] = np.any(~np.isfinite(verts[1:]), axis=1)
                if codes is not None:
                    brk[1:] |= codes[1:] == MplPath.MOVETO
            for idx in np.split(np.arange(len(verts)), np.nonzero(brk)[0]):
                pts = verts[idx][np.all(np.isfinite(verts[idx]), axis=1)]
                if len(pts) < 2:
                    continue
                coords = [rf.transform * (float(px), float(py)) for px, py in pts]
                geoms.append(LineString(coords))
                values.append(float(level))
    finally:
        plt.close(fig)
    if not geoms:
        raise GisError(E_EMPTY_RESULT, "没有提取到等值线（levels 可能都在值域外）")
    out = gpd.GeoDataFrame({"level": values}, geometry=geoms, crs=rf.crs)
    return StepResult(out, [
        f"等值线：{len(set(values))} 个级别 / {len(geoms)} 段线（级别字段 level）",
        f"levels={sorted(set(values))[:8]}{'…' if len(set(values)) > 8 else ''}",
    ])


def handle_sample(resolver, params, step_id):
    rf = _raster_param(resolver, need(params, "raster", "raster.sample"), "sample")
    pts = _vector_param(resolver, need(params, "points", "raster.sample"), "sample.points")
    field_name = str(opt(params, "field", "value"))
    out = pts.reset_index(drop=True).copy()
    vals = []
    inside = 0
    h, w = rf.data.shape
    for geom in out.geometry:
        c, r = ~rf.transform * (geom.x, geom.y)
        ri, ci = int(np.floor(r)), int(np.floor(c))
        if 0 <= ri < h and 0 <= ci < w:
            v = rf.data[ri, ci]
            vals.append(float(v) if np.isfinite(v) else np.nan)
            if np.isfinite(v):
                inside += 1
        else:
            vals.append(np.nan)
    out[field_name] = vals
    return StepResult(out, [
        f"栅格采样：{len(out)} 点，{inside} 点命中有效像元"
        f"（范围外/NoData → NaN）；输出字段 {field_name}",
    ])


SPEC = {
    "raster.info": handle_info,
    "raster.create": handle_create,
    "raster.from_vector": handle_from_vector,
    "raster.align": handle_align,
    "raster.mask": handle_mask,
    "raster.merge": handle_merge,
    "raster.distance": handle_distance,
    "raster.distance_decay": handle_distance_decay,
    "raster.cost_distance": handle_cost_distance,
    "raster.cost_path": handle_cost_path,
    "raster.nearest": handle_nearest,
    "raster.con": handle_con,
    "raster.calc": handle_calc,
    "raster.reclassify": handle_reclassify,
    "raster.breaks": handle_breaks,
    "raster.histogram": handle_histogram,
    "raster.weighted_sum": handle_weighted_sum,
    "raster.focal": handle_focal,
    "raster.zonal_stats": handle_zonal_stats,
    "raster.polygonize": handle_polygonize,
    "raster.contour": handle_contour,
    "raster.sample": handle_sample,
}
