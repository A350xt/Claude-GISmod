"""面板预览：把任意图层（数据集 / 产物 / 中间步骤 / 栅格）渲染成一张「地图卡片」。

与 viz/map.py 的分工：map.py 出正式专题图（PNG、图例、标题），本模块只出面板缩略图：
- SVG 卡片（桌面端）：圆角底板、坐标格网与刻度标注、比例尺、指北针、坐标系徽标，
  内嵌 prefers-color-scheme 样式，亮/暗主题都好看；总长受 SVG_BUDGET 约束；
- PNG（终端图片协议用）：matplotlib 低分辨率快速出图。

坐标系可切换：display_crs ∈ "source" | "utm" | 任意 pyproj 可识别的串（"EPSG:4326"）。
"""

from __future__ import annotations

import base64
import io
import math

import numpy as np

SVG_BUDGET = 120_000          # Svg 元素上限 131072 字符，留余量
WIDTH = 640
MIN_H, MAX_H = 320, 620
PAD = 14                      # 卡片内边距
RADIUS = 14
HEAD = 34                     # 顶部条：坐标系徽标 + 指北针
GUT_L = 50                    # 左侧刻度栏
GUT_B = 22                    # 底部刻度栏

# 亮色为默认，暗色由 SVG 内的媒体查询切换
_STYLE = """
.bg{fill:#F3F4F6}.map{fill:#FFFFFF;stroke:#E2E5EA}.frame{fill:none;stroke:#E2E5EA}
.grid{stroke:#D9DDE3;stroke-width:.6;stroke-dasharray:3 4}
.tick{fill:#8B93A1;font:500 10px ui-sans-serif,system-ui,'Segoe UI','Microsoft YaHei',sans-serif}
.badge{fill:#FFFFFF;stroke:#E2E5EA}.badgeText{fill:#4B5563;font:600 10.5px ui-sans-serif,system-ui,'Segoe UI','Microsoft YaHei',sans-serif}
.poly{fill:#3B82F6;fill-opacity:.26;stroke:#2563EB;stroke-width:.9;stroke-linejoin:round}
.line{fill:none;stroke:#F97316;stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}
.pt{fill:#E11D48;stroke:#FFFFFF;stroke-width:1}
.ink{fill:#374151}.inkStroke{stroke:#374151}.halo{fill:#FFFFFF;fill-opacity:.85}
@media (prefers-color-scheme: dark){
.bg{fill:#141619}.map{fill:#1B1E23;stroke:#2C3139}.frame{stroke:#2A2E35}.grid{stroke:#30353D}.tick{fill:#7D8592}
.badge{fill:#22262C;stroke:#343943}.badgeText{fill:#C9CED6}
.poly{fill:#60A5FA;fill-opacity:.22;stroke:#60A5FA}.line{stroke:#FB923C}.pt{fill:#FB7185;stroke:#17191D}
.ink{fill:#D1D5DB}.inkStroke{stroke:#D1D5DB}.halo{fill:#17191D;fill-opacity:.8}}
"""


# ---------------------------------------------------------------- 坐标系

def resolve_display_crs(crs_in, request: str | None, sample_gdf=None):
    """把请求的显示坐标系解析成 pyproj.CRS；"source"/空 = 源坐标系，"utm" = 按范围选 UTM。"""
    from pyproj import CRS

    from geocore.protocol import E_BAD_REQUEST, GeoCoreError

    req = (request or "source").strip()
    if req.lower() in ("", "source", "源"):
        return CRS.from_user_input(crs_in) if crs_in is not None else None
    if req.lower() == "utm":
        if sample_gdf is not None and len(sample_gdf):
            return sample_gdf.estimate_utm_crs()
        return CRS.from_user_input(crs_in) if crs_in is not None else None
    try:
        return CRS.from_user_input(req)
    except Exception as exc:
        raise GeoCoreError(E_BAD_REQUEST, f"无法识别的坐标系：{req}",
                           {"hint": "可用 source / utm / EPSG:4326 / EPSG:4490 / EPSG:3857 等"}) from exc


def crs_label(crs) -> str:
    if crs is None:
        return "无坐标系"
    epsg = crs.to_epsg()
    name = crs.name.replace("_", " ")
    return f"EPSG:{epsg} · {name}" if epsg else name


def _meters_per_unit(crs, center_lat: float) -> float | None:
    if crs is None:
        return None
    if crs.is_geographic:
        return 111_320.0          # 纬度方向一度（y 轴不做拉伸）
    try:
        factor = float(crs.axis_info[0].unit_conversion_factor)
    except Exception:
        factor = 1.0
    if crs.to_epsg() in (3857, 900913):
        # Web 墨卡托的"米"在纬度 φ 处被放大 1/cosφ；center_lat 此时是投影 y 值
        lat = math.degrees(math.atan(math.sinh(center_lat / 6_378_137.0)))
        factor *= math.cos(math.radians(lat))
    return factor


# ---------------------------------------------------------------- 画布

class _Canvas:
    """世界坐标 → 卡片像素。地理坐标按中心纬度做 cos 校正，避免高纬压扁。

    卡片布局：顶部条（徽标、指北针）| 左刻度栏 + 地图框 | 底刻度栏。
    """

    def __init__(self, bounds, geographic: bool):
        minx, miny, maxx, maxy = bounds
        w = max(maxx - minx, 1e-9)
        h = max(maxy - miny, 1e-9)
        minx, maxx = minx - w * 0.05, maxx + w * 0.05
        miny, maxy = miny - h * 0.05, maxy + h * 0.05
        w, h = maxx - minx, maxy - miny
        self.center_lat = (miny + maxy) / 2
        kx = max(math.cos(math.radians(self.center_lat)), 0.05) if geographic else 1.0
        self.kx = kx
        self.mx0 = PAD + GUT_L
        self.mx1 = WIDTH - PAD
        map_w = self.mx1 - self.mx0
        aspect = h / (w * kx)
        map_h = int(min(MAX_H - HEAD - GUT_B - 2 * PAD, max(MIN_H - HEAD - GUT_B - 2 * PAD,
                                                            round(map_w * aspect))))
        self.my0 = PAD + HEAD
        self.my1 = self.my0 + map_h
        self.height = self.my1 + GUT_B + PAD
        self.scale = min(map_w / (w * kx), map_h / h)
        self.ox = self.mx0 + (map_w - w * kx * self.scale) / 2 - minx * kx * self.scale
        self.oy = self.my0 + (map_h - h * self.scale) / 2 + maxy * self.scale
        self.world_per_px = 1.0 / self.scale
        self.view = (self.wx(self.mx0), self.wy(self.my1), self.wx(self.mx1), self.wy(self.my0))

    def px(self, x: float) -> float:
        return self.ox + x * self.kx * self.scale

    def py(self, y: float) -> float:
        return self.oy - y * self.scale

    def wx(self, px: float) -> float:
        return (px - self.ox) / (self.kx * self.scale)

    def wy(self, py: float) -> float:
        return (self.oy - py) / self.scale

    def pt(self, x: float, y: float) -> str:
        return f"{self.px(x):.1f} {self.py(y):.1f}"


def _nice_step(span: float, target: int = 4) -> float:
    raw = span / max(target, 1)
    if raw <= 0:
        return 1.0
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


def _fmt_tick(v: float, geographic: bool, axis: str, step: float) -> str:
    if geographic:
        hemi = ("E" if v >= 0 else "W") if axis == "x" else ("N" if v >= 0 else "S")
        decimals = max(0, min(4, -int(math.floor(math.log10(step))) if step < 1 else 0))
        return f"{abs(v):.{decimals}f}°{hemi}"
    if abs(step) >= 1000:
        return f"{v / 1000:,.0f} km" if step >= 1000 else f"{v / 1000:,.1f} km"
    return f"{v:,.0f}"


def _decor(c: _Canvas, crs, title: str) -> tuple[list[str], list[str]]:
    """返回 (底层：地图框与格网, 顶层：刻度/比例尺/指北针/徽标)。"""
    geographic = bool(crs is not None and crs.is_geographic)
    under: list[str] = [f'<rect class="map" x="{c.mx0}" y="{c.my0}" width="{c.mx1 - c.mx0}" '
                        f'height="{c.my1 - c.my0}" rx="8"/>']
    over: list[str] = []
    vminx, vminy, vmaxx, vmaxy = c.view

    # 格网 + 刻度（标注放在地图框外的刻度栏里）
    if crs is not None:
        sx = _nice_step(vmaxx - vminx, 4)
        sy = _nice_step(vmaxy - vminy, 4)
        grid = []
        x = math.ceil(vminx / sx) * sx
        while x <= vmaxx:
            px = c.px(x)
            if c.mx0 + 4 < px < c.mx1 - 4:
                grid.append(f"M{px:.1f} {c.my0}V{c.my1}")
                over.append(f'<text class="tick" x="{px:.1f}" y="{c.my1 + 15}" '
                            f'text-anchor="middle">{_fmt_tick(x, geographic, "x", sx)}</text>')
            x += sx
        y = math.ceil(vminy / sy) * sy
        while y <= vmaxy:
            py = c.py(y)
            if c.my0 + 4 < py < c.my1 - 4:
                grid.append(f"M{c.mx0} {py:.1f}H{c.mx1}")
                over.append(f'<text class="tick" x="{c.mx0 - 6}" y="{py + 3.5:.1f}" '
                            f'text-anchor="end">{_fmt_tick(y, geographic, "y", sy)}</text>')
            y += sy
        under.append(f'<path class="grid" d="{"".join(grid)}"/>')

    # 比例尺（地图框右下角）
    mpu = _meters_per_unit(crs, c.center_lat)
    if mpu:
        m_per_px = c.world_per_px * mpu
        nice_m = _nice_step(m_per_px * (c.mx1 - c.mx0) * 0.2, 1)
        bar_px = nice_m / m_per_px
        label = f"{nice_m / 1000:g} km" if nice_m >= 1000 else f"{nice_m:g} m"
        x1 = c.mx1 - 14
        x0 = x1 - bar_px
        yb = c.my1 - 12
        over.append(f'<rect class="halo" x="{x0 - 10:.1f}" y="{yb - 20}" width="{bar_px + 20:.1f}" '
                    f'height="28" rx="6"/>')
        over.append(f'<path class="inkStroke" fill="none" stroke-width="1.5" '
                    f'd="M{x0:.1f} {yb - 4}V{yb}H{x1:.1f}V{yb - 4}"/>')
        over.append(f'<text class="tick" x="{(x0 + x1) / 2:.1f}" y="{yb - 8}" text-anchor="middle">'
                    f'{label}</text>')

    # 顶部条：坐标系徽标（左）+ 指北针（右）
    label = _esc(crs_label(crs))
    bw = 22 + 6.2 * len(label)
    over.append(f'<rect class="badge" x="{c.mx0}" y="{PAD + 2}" width="{bw:.0f}" height="22" rx="11"/>'
                f'<circle cx="{c.mx0 + 11}" cy="{PAD + 13}" r="3" fill="#10B981"/>'
                f'<text class="badgeText" x="{c.mx0 + 19}" y="{PAD + 17}">{label}</text>')
    nx, ny = c.mx1 - 10, PAD + 13
    over.append(f'<g transform="translate({nx},{ny})">'
                f'<path class="ink" d="M0 -9L4.5 5L0 2.5L-4.5 5Z"/>'
                f'<text class="tick" x="-9" y="4" text-anchor="end" font-weight="700">N</text></g>')
    return under, over


def _card(c: _Canvas, title: str, under: list[str], body: list[str], over: list[str]) -> str:
    return "".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {c.height}" '
        f'width="{WIDTH}" height="{c.height}">',
        f"<title>{_esc(title)}</title><style>{_STYLE}</style>",
        f'<defs><clipPath id="clip"><rect x="0" y="0" width="{WIDTH}" height="{c.height}" '
        f'rx="{RADIUS}"/></clipPath></defs>',
        f'<g clip-path="url(#clip)"><rect class="bg" width="{WIDTH}" height="{c.height}"/>',
        *under, *body, *over,
        "</g>",
        f'<rect class="frame" x=".5" y=".5" width="{WIDTH - 1}" height="{c.height - 1}" rx="{RADIUS}"/>',
        "</svg>",
    ])


# ---------------------------------------------------------------- 矢量

def _ring_path(coords, c: _Canvas, close: bool) -> str:
    pts = list(coords)
    if len(pts) < 2:
        return ""
    body = " L".join(c.pt(x, y) for x, y, *_ in pts)
    return f"M{body}{'Z' if close else ''}"


def _geom_paths(geom, c: _Canvas, out: dict[str, list[str]]) -> None:
    if geom is None or geom.is_empty:
        return
    t = geom.geom_type
    if t == "Polygon":
        d = _ring_path(geom.exterior.coords, c, True)
        d += "".join(_ring_path(r.coords, c, True) for r in geom.interiors)
        if d:
            out["poly"].append(d)
    elif t in ("LineString", "LinearRing"):
        d = _ring_path(geom.coords, c, False)
        if d:
            out["line"].append(d)
    elif t == "Point":
        out["point"].append(f'<circle cx="{c.px(geom.x):.1f}" cy="{c.py(geom.y):.1f}" r="3.6"/>')
    elif hasattr(geom, "geoms"):
        for g in geom.geoms:
            _geom_paths(g, c, out)


def _svg_vector(gdf, title: str, tolerance_px: float) -> str:
    geographic = bool(gdf.crs is not None and gdf.crs.is_geographic)
    c = _Canvas(tuple(float(v) for v in gdf.total_bounds), geographic)
    tol = c.world_per_px * tolerance_px
    out: dict[str, list[str]] = {"poly": [], "line": [], "point": []}
    for geom in gdf.geometry:
        if geom is None or geom.is_empty:
            continue
        if tol > 0 and geom.geom_type not in ("Point", "MultiPoint"):
            geom = geom.simplify(tol, preserve_topology=False)
        _geom_paths(geom, c, out)
    body = []
    if out["poly"]:
        body.append(f'<path class="poly" fill-rule="evenodd" d="{" ".join(out["poly"])}"/>')
    if out["line"]:
        body.append(f'<path class="line" d="{" ".join(out["line"])}"/>')
    if out["point"]:
        body.append(f'<g class="pt">{"".join(out["point"])}</g>')
    under, over = _decor(c, gdf.crs, title)
    return _card(c, title, under, body, over)


def _esc(s: str) -> str:
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def vector_svg(gdf, title: str = "") -> str:
    """矢量 → SVG 卡片。超预算时逐级放大化简容差，最后退化为抽样点。"""
    if len(gdf) == 0 or gdf.geometry.is_empty.all():
        return _empty_svg("空图层")
    for tol in (0.6, 1.5, 4.0, 10.0):
        svg = _svg_vector(gdf, title, tol)
        if len(svg) <= SVG_BUDGET:
            return svg
    reps = gdf.copy()
    reps["geometry"] = gdf.geometry.representative_point()
    step = max(1, len(reps) // 2500)
    return _svg_vector(reps.iloc[::step], title + "（抽样）", 0)


def _empty_svg(text: str) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {MIN_H}" '
            f'width="{WIDTH}" height="{MIN_H}"><style>{_STYLE}</style>'
            f'<rect class="bg" width="{WIDTH}" height="{MIN_H}" rx="{RADIUS}"/>'
            f'<text class="tick" x="50%" y="50%" text-anchor="middle" font-size="14">{_esc(text)}</text></svg>')


# ---------------------------------------------------------------- 栅格

def reproject_raster(rf, dst_crs):
    """把栅格重投影到 dst_crs（像元数不变量级）；同 CRS 原样返回。"""
    from pyproj import CRS

    from geocore.raster_core import RasterFrame, reproject_rf

    if dst_crs is None or rf.crs is None or CRS.from_user_input(rf.crs) == dst_crs:
        return rf
    from rasterio.warp import calculate_default_transform

    h, w = rf.data.shape
    minx, miny, maxx, maxy = rf.bounds
    transform, width, height = calculate_default_transform(rf.crs, dst_crs, w, h, minx, miny, maxx, maxy)
    out = reproject_rf(rf, dst_crs, transform=transform, shape=(height, width))
    return RasterFrame(out.data, transform, dst_crs, name=rf.name)


def _raster_png(data: np.ndarray, max_side: int = 420, cmap: str = "viridis") -> bytes:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    arr = np.asarray(data, dtype="float64")
    step = max(1, int(math.ceil(max(arr.shape) / max_side)))
    arr = arr[::step, ::step]
    finite = arr[np.isfinite(arr)]
    lo, hi = (np.percentile(finite, [2, 98]) if finite.size else (0.0, 1.0))
    buf = io.BytesIO()
    plt.imsave(buf, np.ma.masked_invalid(arr), cmap=cmap, vmin=lo, vmax=hi if hi > lo else lo + 1,
               format="png")
    return buf.getvalue()


def raster_svg(rf, title: str = "") -> tuple[str, bytes]:
    """栅格 → (SVG 卡片内嵌 PNG, PNG 字节)。"""
    from pyproj import CRS

    crs = CRS.from_user_input(rf.crs) if rf.crs is not None else None
    c = _Canvas(tuple(float(v) for v in rf.bounds), bool(crs is not None and crs.is_geographic))
    under, over = _decor(c, crs, title)
    minx, miny, maxx, maxy = rf.bounds
    x0, y0, x1, y1 = c.px(minx), c.py(maxy), c.px(maxx), c.py(miny)
    side = 420
    while True:
        png = _raster_png(rf.data, side)
        b64 = base64.b64encode(png).decode("ascii")
        if len(b64) < SVG_BUDGET - 6000 or side <= 64:
            break
        side = int(side * 0.7)
    body = [f'<image x="{x0:.1f}" y="{y0:.1f}" width="{x1 - x0:.1f}" height="{y1 - y0:.1f}" '
            f'preserveAspectRatio="none" style="image-rendering:pixelated" '
            f'href="data:image/png;base64,{b64}"/>']
    return _card(c, title, under, body, over), png


# ---------------------------------------------------------------- 终端 PNG

def vector_png(gdf, size_px: int = 560) -> bytes:
    """矢量 → 小 PNG（终端图片协议用）。透明底，配色与 SVG 一致。"""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(size_px / 100, size_px * 0.75 / 100), dpi=100)
    try:
        fig.patch.set_alpha(0)
        ax.set_facecolor("none")
        ax.grid(True, linestyle=(0, (3, 4)), linewidth=0.5, color="#9AA3AF", alpha=0.6)
        ax.tick_params(labelsize=7, colors="#8B93A1", length=0)
        for side in ax.spines.values():
            side.set_visible(False)
        gt = gdf.geometry.geom_type
        polys = gdf[gt.isin(["Polygon", "MultiPolygon"])]
        lines = gdf[gt.isin(["LineString", "MultiLineString"])]
        pts = gdf[gt.isin(["Point", "MultiPoint"])]
        if len(polys):
            polys.plot(ax=ax, facecolor="#3B82F6", alpha=0.3, edgecolor="#2563EB", linewidth=0.7)
        if len(lines):
            lines.plot(ax=ax, color="#F97316", linewidth=1.3)
        if len(pts):
            pts.plot(ax=ax, color="#E11D48", markersize=12, edgecolor="white", linewidth=0.5)
        if gdf.crs is not None and gdf.crs.is_geographic and len(gdf):
            lat = float(np.mean(gdf.total_bounds[1::2]))
            ax.set_aspect(1 / max(math.cos(math.radians(lat)), 0.05))
        else:
            ax.set_aspect("equal")
        buf = io.BytesIO()
        fig.savefig(buf, format="png", bbox_inches="tight", pad_inches=0.05, transparent=True)
        return buf.getvalue()
    finally:
        plt.close(fig)
