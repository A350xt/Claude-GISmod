"""版面合成渲染器（制图模式的服务端出口）。

浏览器端是版面规格（layout spec）编辑器；导出/预览都由本模块用同一份
matplotlib 渲染管线完成——预览即成品，天然所见即所得。

spec 结构（坐标一律 0..1，y 从页面顶部量起；页面 A4/A3/A2 或自定义 mm）：
{
  "title": "…",
  "page": {"size": "A4", "orientation": "landscape", "width_mm?": .., "height_mm?": ..},
  "elements": [
    {"id": "map1", "type": "map", "x": .05, "y": .08, "w": .62, "h": .8,
     "extent": [minx,miny,maxx,maxy]?,          # 缺省取图层并集
     "layers": [                                  # 自下而上叠加
       {"source": "…", "label": "行政区", "mode": "single", "color": "#3b82f6", "alpha": .5},
       {"source": "…", "mode": "categorical", "field": "grade", "category_colors": {…}},
       {"source": "….tif", "mode": "continuous", "cmap": "YlOrRd"}]}
    {"id": "t",  "type": "title", "x": .05, "y": .02, "w": .9, "h": .06, "text": "…"},
    {"id": "s",  "type": "text",  …, "text": "多行\\n文本", "size": 7, "align": "left"},
    {"id": "l",  "type": "legend"},
    {"id": "sb", "type": "scalebar", "segments": 4},
    {"id": "na", "type": "north"},
  ]
}
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyArrow, Patch, Rectangle  # noqa: E402

from claude_gis.viz.map import _fmt_val  # noqa: E402

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "Noto Sans CJK SC", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

PAGE_SIZES = {  # mm（竖版）
    "A4": (210, 297),
    "A3": (297, 420),
    "A2": (420, 594),
    "LETTER": (216, 279),
}


def _page_size(page: dict) -> tuple[float, float]:
    size = str(page.get("size", "A4")).upper()
    if size == "CUSTOM":
        w = float(page.get("width_mm", 210))
        h = float(page.get("height_mm", 297))
    else:
        w, h = PAGE_SIZES.get(size, PAGE_SIZES["A4"])
    if str(page.get("orientation", "portrait")).lower() == "landscape":
        w, h = h, w
    return w, h


def _rect(el: dict) -> tuple[float, float, float, float]:
    """spec 坐标（y 向下）→ matplotlib axes rect（y 向上）。"""
    x, y = float(el.get("x", .05)), float(el.get("y", .05))
    w, h = float(el.get("w", .3)), float(el.get("h", .3))
    return (x, 1.0 - y - h, w, h)


def _load_sources(store, layers: list):
    """图层引用 → (gdf | RasterFrame, style)。复用 resolver 的 CRS 对齐。"""
    from claude_gis.analysis.resolver import DatasetResolver
    from claude_gis.raster_core import RasterFrame

    resolver = DatasetResolver(store, crs_override="source")
    out = []
    for spec in layers or []:
        tok = str(spec.get("source") or "")
        if not tok:
            continue
        try:
            try:
                rf, _ = resolver.raster(tok)
                out.append((rf, spec))
                continue
            except Exception:
                gdf, _ = resolver.frame(tok)
                out.append((gdf, spec))
        except Exception as exc:
            resolver.warnings.append(f"图层 {tok} 加载失败，已跳过：{exc}")
    return out, resolver.warnings


def _plot_layer(ax, obj, spec: dict):
    import geopandas as gpd
    import numpy as np

    from claude_gis.raster_core import RasterFrame
    from claude_gis.viz.map import _fmt_val

    mode = str(spec.get("mode", "single")).lower()
    alpha = float(spec.get("alpha", 1.0))
    if isinstance(obj, RasterFrame):
        data = np.ma.masked_invalid(obj.data)
        minx, miny, maxx, maxy = obj.bounds
        if mode in ("categorical", "category"):
            values = np.unique(obj.data[np.isfinite(obj.data)])
            import matplotlib.colors as mcolors

            overrides = spec.get("category_colors") or {}
            palette = [overrides.get(str(_fmt_val(v)), None) or tuple(float(c) for c in col)
                       for v, col in zip(values, plt.get_cmap("tab20")(range(len(values))))]
            cmap = mcolors.ListedColormap(palette)
            norm = mcolors.BoundaryNorm(np.append(values - .5, values[-1] + .5), cmap.N)
            ax.imshow(data, cmap=cmap, norm=norm, extent=(minx, maxx, miny, maxy),
                      origin="upper", interpolation="nearest", alpha=alpha)
        else:
            finite = obj.data[np.isfinite(obj.data)]
            lo, hi = (np.percentile(finite, [2, 98]) if finite.size else (0, 1))
            ax.imshow(data, cmap=str(spec.get("cmap", "viridis")), vmin=lo, vmax=hi,
                      extent=(minx, maxx, miny, maxy), origin="upper", alpha=alpha)
        return

    gdf: gpd.GeoDataFrame = obj
    color = str(spec.get("color", "#3b82f6"))
    if mode in ("categorical", "category"):
        field = spec.get("field")
        if field and field in gdf.columns:
            filled = gdf[field].fillna("<空>").astype(str)
            cats = sorted(filled.unique())
            overrides = spec.get("category_colors") or {}
            cmap = plt.get_cmap("tab20", max(len(cats), 1))
            colors = {c: overrides.get(c) or cmap(i % 20) for i, c in enumerate(cats)}
            gdf.plot(ax=ax, color=[colors[c] for c in filled], alpha=alpha,
                     edgecolor="white", linewidth=.4, zorder=2)
        else:
            gdf.plot(ax=ax, color=color, alpha=alpha, zorder=2)
    elif mode in ("choropleth", "graded"):
        field = spec.get("field")
        if field and field in gdf.columns:
            gdf.plot(ax=ax, column=field, cmap=str(spec.get("cmap", "YlOrRd")),
                     alpha=alpha, edgecolor="white", linewidth=.4, zorder=2,
                     missing_kwds={"color": "#cccccc"})
        else:
            gdf.plot(ax=ax, color=color, alpha=alpha, zorder=2)
    else:
        fam_point = set(map(str, gdf.geometry.geom_type)) <= {"Point", "MultiPoint"}
        if fam_point:
            gdf.plot(ax=ax, color=color, markersize=float(spec.get("size", 18)),
                     edgecolor="white", linewidth=.4, alpha=alpha, zorder=3)
        else:
            gdf.plot(ax=ax, color=color, alpha=min(alpha, .8),
                     edgecolor="white", linewidth=.5, zorder=2)


def _legend_items(store, layers: list) -> list:
    """(label, 颜色或渐变说明) 列表：single→色块；categorical→逐类；raster→色带说明。"""
    import numpy as np

    from claude_gis.raster_core import RasterFrame, load_raster
    from claude_gis.viz.map import _fmt_val

    items = []
    for spec in layers or []:
        label = str(spec.get("label") or spec.get("source", "?"))
        mode = str(spec.get("mode", "single")).lower()
        try:
            if str(spec.get("source", "")).lower().endswith((".tif", ".tiff")):
                rf = load_raster(spec["source"])
                if mode in ("categorical", "category"):
                    for v in np.unique(rf.data[np.isfinite(rf.data)]):
                        items.append((f"{label}·{_fmt_val(v)}", "auto-categorical"))
                else:
                    items.append((f"{label}（{spec.get('cmap', 'viridis')} 色带）", "ramp"))
                continue
            gdf, _ = __import__("claude_gis.runtime.datasource", fromlist=["load_dataset"]).load_dataset(
                str(spec["source"]))
            if mode in ("categorical", "category") and spec.get("field") in gdf.columns:
                for c in sorted(gdf[spec["field"]].fillna("<空>").astype(str).unique()):
                    items.append((f"{label}·{c}", "auto-categorical"))
            elif mode in ("choropleth", "graded") and spec.get("field") in gdf.columns:
                items.append((f"{label}（{spec['field']} 分级）", "ramp"))
            else:
                items.append((label, str(spec.get("color", "#3b82f6"))))
        except Exception:
            items.append((label, str(spec.get("color", "#3b82f6"))))
    return items


def _deco_axes(fig, rect) -> "matplotlib.axes.Axes":
    """装饰要素（标题/文本/图例/比例尺/指北针）专用 axes。

    关键：关闭 autoscale 并钉死 0..1 数据域——否则后续 add_patch 的
    data-extent 会把 axes 缩放拉扯变形（图例压缩成黑块的根因）。
    """
    ax = fig.add_axes(rect)
    ax.axis("off")
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.set_autoscale_on(False)
    return ax


def render_composition(store, spec: dict, *, out_path: Path, dpi: int = 200) -> dict:
    """渲染整幅版面；返回 {image_path, warnings}。"""
    page = dict(spec.get("page") or {})
    w_mm, h_mm = _page_size(page)
    fig = plt.figure(figsize=(w_mm / 25.4, h_mm / 25.4))
    warnings: list[str] = []
    elements = spec.get("elements") or []

    map_elements = [e for e in elements if e.get("type") == "map"]
    loaded: dict[int, tuple] = {}
    for i, el in enumerate(map_elements):
        objs, w = _load_sources(store, el.get("layers"))
        loaded[i] = objs
        warnings.extend(w)

    # ---- 地图框 ----
    for i, el in enumerate(map_elements):
        ax = fig.add_axes(_rect(el))
        objs = loaded.get(i) or []
        if not objs:
            ax.text(.5, .5, "（无图层）", ha="center", va="center",
                    transform=ax.transAxes, fontsize=9, color="#94a3b8")
        for obj, layer_spec in objs:
            try:
                _plot_layer(ax, obj, layer_spec)
            except Exception as exc:
                warnings.append(f"图层 {layer_spec.get('source')} 绘制失败，已跳过：{exc}")
        # 范围
        extent = el.get("extent")
        if extent and len(extent) == 4:
            ax.set_xlim(extent[0], extent[2])
            ax.set_ylim(extent[1], extent[3])
        else:
            import geopandas as gpd

            from claude_gis.raster_core import RasterFrame

            bounds = []
            for obj, _ in objs:
                if isinstance(obj, RasterFrame):
                    bounds.append(obj.bounds)
                else:
                    b = obj.total_bounds
                    bounds.append((b[0], b[1], b[2], b[3]))
            if bounds:
                minx = min(b[0] for b in bounds)
                miny = min(b[1] for b in bounds)
                maxx = max(b[2] for b in bounds)
                maxy = max(b[3] for b in bounds)
                pad_x = (maxx - minx) * .03 or 1
                pad_y = (maxy - miny) * .03 or 1
                ax.set_xlim(minx - pad_x, maxx + pad_x)
                ax.set_ylim(miny - pad_y, maxy + pad_y)
        ax.set_facecolor("#ffffff")
        for spine in ax.spines.values():
            spine.set_linewidth(.8)
            spine.set_color("#0f1115")

    # ---- 标题 / 文本 ----
    for el in elements:
        if el.get("type") == "title":
            ax = _deco_axes(fig, _rect(el))
            ax.text(0.5, 0.5, str(el.get("text", "")), ha="center", va="center",
                    fontsize=float(el.get("size", 16)), fontweight="bold",
                    transform=ax.transAxes)
        elif el.get("type") == "text":
            ax = _deco_axes(fig, _rect(el))
            align = str(el.get("align", "left"))
            ax.text(0.0 if align == "left" else (1.0 if align == "right" else .5), .5,
                    str(el.get("text", "")),
                    ha=align, va="center", fontsize=float(el.get("size", 7)),
                    linespacing=1.6, color=str(el.get("color", "#334155")),
                    transform=ax.transAxes)

    # ---- 图例 ----
    for el in elements:
        if el.get("type") != "legend":
            continue
        target = el.get("for") or (map_elements[0].get("id") if map_elements else None)
        layers = []
        for m in map_elements:
            if target in (None, m.get("id")):
                layers.extend(m.get("layers") or [])
        ax = _deco_axes(fig, _rect(el))
        ax.add_patch(Rectangle((0, 0), 1, 1, transform=ax.transAxes, fill=False,
                               linewidth=.6, edgecolor="#cbd5e1"))
        items = _legend_items(store, layers)
        y = .93
        title = str(el.get("title") or "图例")
        ax.text(.08, y, title, fontsize=float(el.get("size", 8)), fontweight="bold",
                va="top", transform=ax.transAxes)
        y -= .12
        for label, color in items[:16]:
            if color == "ramp":
                swatch = "#fde047"
            elif color == "auto-categorical":
                swatch = "#94a3b8"
            else:
                swatch = color
            ax.add_patch(Rectangle((.08, y - .028), .16, .05, transform=ax.transAxes,
                                   facecolor=swatch, edgecolor="#94a3b8", linewidth=.4))
            ax.text(.30, y, label, fontsize=float(el.get("size", 7)) - .5,
                    va="center", transform=ax.transAxes)
            y -= .095
            if y < .04:
                break

    # ---- 比例尺（挂在所属地图框的横向比例上） ----
    for el in elements:
        if el.get("type") != "scalebar":
            continue
        target = el.get("for") or (map_elements[0].get("id") if map_elements else None)
        m = next((m for m in map_elements if m.get("id") == target), map_elements[0] if map_elements else None)
        ax = _deco_axes(fig, _rect(el))
        if m is None or not m.get("extent"):
            ax.text(0, .5, "（需地图框定范围）", fontsize=6, va="center",
                    transform=ax.transAxes, color="#94a3b8")
            continue
        minx, miny, maxx, maxy = (float(v) for v in m["extent"])
        # 横向度→米：地理坐标系按中纬度近似，投影直接是米
        width_x = maxx - minx
        if width_x <= 180.5 and abs(miny) <= 90.5:  # 视作经纬度
            import math

            mid_lat = (miny + maxy) / 2
            width_m = width_x * 111_320 * math.cos(math.radians(mid_lat))
            unit = "km"
            nice_km = _nice_value(width_m / 1000 / 4)
            bar_label = f"{_fmt_val(nice_km)} km"
            bar_km = nice_km
        else:
            width_m = width_x
            nice_m = _nice_value(width_m / 4)
            bar_label = f"{_fmt_val(nice_m / 1000) if nice_m >= 1000 else _fmt_val(nice_m)} " + ("km" if nice_m >= 1000 else "m")
            bar_km = nice_m / 1000
        frac = (bar_km * 1000) / width_m if width_m else .5
        frac = min(max(frac, .05), .98)
        segments = int(el.get("segments", 4))
        seg_w = frac / segments
        for i in range(segments):
            ax.add_patch(Rectangle((i * seg_w, .35), seg_w, .3,
                                   transform=ax.transAxes,
                                   facecolor="#0f1115" if i % 2 == 0 else "#ffffff",
                                   edgecolor="#0f1115", linewidth=.7))
        ax.text(0, .05, "0", fontsize=6, transform=ax.transAxes)
        ax.text(frac, .05, bar_label, fontsize=6, ha="right", transform=ax.transAxes)

    # ---- 指北针（默认北向上） ----
    for el in elements:
        if el.get("type") != "north":
            continue
        ax = _deco_axes(fig, _rect(el))
        ax.add_patch(FancyArrow(.5, .42, 0, .38, transform=ax.transAxes,
                                width=.10, head_width=.34, head_length=.22,
                                length_includes_head=True, facecolor="#0f1115"))
        ax.text(.5, .16, "N", ha="center", fontsize=float(el.get("size", 10)),
                fontweight="bold", transform=ax.transAxes)

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=dpi, facecolor="white")
    plt.close(fig)
    return {"warnings": warnings}


def _nice_value(x: float) -> float:
    """取 1/2/5×10^n 形态的整数刻度值。"""
    if x <= 0:
        return 1.0
    import math

    exp = math.floor(math.log10(x))
    base = x / (10 ** exp)
    for n in (1, 2, 5, 10):
        if base <= n + 1e-9:
            return float(n * (10 ** exp))
    return float(10 ** (exp + 1))
