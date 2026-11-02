#include "preview.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <cpl_vsi.h>
#include <gdal_alg.h>
#include <gdal_priv.h>
#include <ogr_api.h>

namespace gis {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int WIDTH = 640;
constexpr int MIN_H = 320, MAX_H = 620;
constexpr int PAD = 14, RADIUS = 14, HEAD = 34, GUT_L = 50, GUT_B = 22;

const char* STYLE = R"CSS(
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
)CSS";

std::string f1(double v) { return fmt_fixed(v, 1); }

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

size_t char_count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

/// 世界坐标 → 卡片像素；地理坐标按中心纬度做 cos 校正
struct Canvas {
    double kx = 1, scale = 1, ox = 0, oy = 0, world_per_px = 1, center_lat = 0;
    int mx0 = PAD + GUT_L, mx1 = WIDTH - PAD, my0 = PAD + HEAD, my1 = 0, height = 0;
    Bounds view{};

    Canvas(Bounds b, bool geographic) {
        double w = std::max(b[2] - b[0], 1e-9), h = std::max(b[3] - b[1], 1e-9);
        double minx = b[0] - w * 0.05, maxx = b[2] + w * 0.05, miny = b[1] - h * 0.05, maxy = b[3] + h * 0.05;
        w = maxx - minx;
        h = maxy - miny;
        center_lat = (miny + maxy) / 2;
        kx = geographic ? std::max(std::cos(center_lat * PI / 180.0), 0.05) : 1.0;
        const int map_w = mx1 - mx0;
        const double aspect = h / (w * kx);
        const int lo = MIN_H - HEAD - GUT_B - 2 * PAD, hi = MAX_H - HEAD - GUT_B - 2 * PAD;
        const int map_h = std::clamp(static_cast<int>(std::lround(map_w * aspect)), lo, hi);
        my1 = my0 + map_h;
        height = my1 + GUT_B + PAD;
        scale = std::min(map_w / (w * kx), map_h / h);
        ox = mx0 + (map_w - w * kx * scale) / 2 - minx * kx * scale;
        oy = my0 + (map_h - h * scale) / 2 + maxy * scale;
        world_per_px = 1.0 / scale;
        view = {wx(mx0), wy(my1), wx(mx1), wy(my0)};
    }
    double px(double x) const { return ox + x * kx * scale; }
    double py(double y) const { return oy - y * scale; }
    double wx(double p) const { return (p - ox) / (kx * scale); }
    double wy(double p) const { return (oy - p) / scale; }
    std::string pt(double x, double y) const { return f1(px(x)) + " " + f1(py(y)); }
};

double nice_step(double span, int target) {
    const double raw = span / std::max(target, 1);
    if (!(raw > 0)) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10 * mag;
}

std::string fmt_tick(double v, bool geographic, bool is_x, double step) {
    if (geographic) {
        const char* hemi = is_x ? (v >= 0 ? "E" : "W") : (v >= 0 ? "N" : "S");
        const int decimals = step < 1 ? std::clamp(-static_cast<int>(std::floor(std::log10(step))), 0, 4) : 0;
        return fmt_fixed(std::fabs(v), decimals) + "°" + hemi;
    }
    if (std::fabs(step) >= 1000) return fmt_thousands(v / 1000) + " km";
    return fmt_thousands(v);
}

double meters_per_unit(const Srs& srs, double center) {
    if (!srs) return 0;
    if (srs->IsGeographic()) return 111'320.0;
    double factor = srs->GetLinearUnits(nullptr);
    const int code = srs_epsg_code(srs);
    if (code == 3857 || code == 900913) {  // Web 墨卡托的"米"在纬度 φ 处被放大 1/cosφ
        const double lat = std::atan(std::sinh(center / 6378137.0));
        factor *= std::cos(lat);
    }
    return factor;
}

void decor(const Canvas& c, const Srs& srs, std::string& under, std::string& over) {
    const bool geographic = srs && srs->IsGeographic();
    under += "<rect class=\"map\" x=\"" + std::to_string(c.mx0) + "\" y=\"" + std::to_string(c.my0) + "\" width=\"" +
             std::to_string(c.mx1 - c.mx0) + "\" height=\"" + std::to_string(c.my1 - c.my0) + "\" rx=\"8\"/>";
    if (srs) {
        const double sx = nice_step(c.view[2] - c.view[0], 4), sy = nice_step(c.view[3] - c.view[1], 4);
        std::string grid;
        for (double x = std::ceil(c.view[0] / sx) * sx; x <= c.view[2]; x += sx) {
            const double px = c.px(x);
            if (!(c.mx0 + 4 < px && px < c.mx1 - 4)) continue;
            grid += "M" + f1(px) + " " + std::to_string(c.my0) + "V" + std::to_string(c.my1);
            over += "<text class=\"tick\" x=\"" + f1(px) + "\" y=\"" + std::to_string(c.my1 + 15) + "\" text-anchor=\"middle\">" +
                    fmt_tick(x, geographic, true, sx) + "</text>";
        }
        for (double y = std::ceil(c.view[1] / sy) * sy; y <= c.view[3]; y += sy) {
            const double py = c.py(y);
            if (!(c.my0 + 4 < py && py < c.my1 - 4)) continue;
            grid += "M" + std::to_string(c.mx0) + " " + f1(py) + "H" + std::to_string(c.mx1);
            over += "<text class=\"tick\" x=\"" + std::to_string(c.mx0 - 6) + "\" y=\"" + f1(py + 3.5) + "\" text-anchor=\"end\">" +
                    fmt_tick(y, geographic, false, sy) + "</text>";
        }
        under += "<path class=\"grid\" d=\"" + grid + "\"/>";
    }
    if (const double mpu = meters_per_unit(srs, c.center_lat); mpu > 0) {
        const double m_per_px = c.world_per_px * mpu;
        const double nice_m = nice_step(m_per_px * (c.mx1 - c.mx0) * 0.2, 1);
        const double bar = nice_m / m_per_px;
        const std::string label = nice_m >= 1000 ? fmt_g(nice_m / 1000) + " km" : fmt_g(nice_m) + " m";
        const double x1 = c.mx1 - 14, x0 = x1 - bar;
        const int yb = c.my1 - 12;
        over += "<rect class=\"halo\" x=\"" + f1(x0 - 10) + "\" y=\"" + std::to_string(yb - 20) + "\" width=\"" + f1(bar + 20) +
                "\" height=\"28\" rx=\"6\"/>";
        over += "<path class=\"inkStroke\" fill=\"none\" stroke-width=\"1.5\" d=\"M" + f1(x0) + " " + std::to_string(yb - 4) +
                "V" + std::to_string(yb) + "H" + f1(x1) + "V" + std::to_string(yb - 4) + "\"/>";
        over += "<text class=\"tick\" x=\"" + f1((x0 + x1) / 2) + "\" y=\"" + std::to_string(yb - 8) +
                "\" text-anchor=\"middle\">" + label + "</text>";
    }
    const std::string label = esc(srs_label(srs));
    const double bw = 22 + 6.2 * char_count(label);
    over += "<rect class=\"badge\" x=\"" + std::to_string(c.mx0) + "\" y=\"" + std::to_string(PAD + 2) + "\" width=\"" +
            std::to_string(static_cast<int>(std::lround(bw))) + "\" height=\"22\" rx=\"11\"/><circle cx=\"" +
            std::to_string(c.mx0 + 11) + "\" cy=\"" + std::to_string(PAD + 13) +
            "\" r=\"3\" fill=\"#10B981\"/><text class=\"badgeText\" x=\"" + std::to_string(c.mx0 + 19) + "\" y=\"" +
            std::to_string(PAD + 17) + "\">" + label + "</text>";
    over += "<g transform=\"translate(" + std::to_string(c.mx1 - 10) + "," + std::to_string(PAD + 13) +
            ")\"><path class=\"ink\" d=\"M0 -9L4.5 5L0 2.5L-4.5 5Z\"/><text class=\"tick\" x=\"-9\" y=\"4\" "
            "text-anchor=\"end\" font-weight=\"700\">N</text></g>";
}

std::string card(const Canvas& c, const std::string& title, const std::string& under, const std::string& body,
                 const std::string& over) {
    const std::string W = std::to_string(WIDTH), H = std::to_string(c.height);
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + W + " " + H + "\" width=\"" + W + "\" height=\"" + H +
           "\"><title>" + esc(title) + "</title><style>" + STYLE + "</style><defs><clipPath id=\"clip\"><rect x=\"0\" y=\"0\" width=\"" +
           W + "\" height=\"" + H + "\" rx=\"" + std::to_string(RADIUS) + "\"/></clipPath></defs><g clip-path=\"url(#clip)\"><rect class=\"bg\" width=\"" +
           W + "\" height=\"" + H + "\"/>" + under + body + over + "</g><rect class=\"frame\" x=\".5\" y=\".5\" width=\"" +
           std::to_string(WIDTH - 1) + "\" height=\"" + std::to_string(c.height - 1) + "\" rx=\"" + std::to_string(RADIUS) +
           "\"/></svg>";
}

// ---------------------------------------------------------------- 矢量

void ring_path(const OGRSimpleCurve* curve, const Canvas& c, bool close, std::string& out) {
    const int n = curve->getNumPoints();
    if (n < 2) return;
    out += "M";
    for (int i = 0; i < n; ++i) {
        if (i) out += " L";
        out += c.pt(curve->getX(i), curve->getY(i));
    }
    if (close) out += "Z";
}

void geom_paths(const OGRGeometry* g, const Canvas& c, std::string& poly, std::string& line, std::string& pts) {
    if (!g || g->IsEmpty()) return;
    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPolygon: {
            const OGRPolygon* p = g->toPolygon();
            for (const auto* ring : *p) ring_path(ring, c, true, poly);
            break;
        }
        case wkbLineString:
        case wkbLinearRing: ring_path(g->toSimpleCurve(), c, false, line); break;
        case wkbPoint: {
            const OGRPoint* p = g->toPoint();
            pts += "<circle cx=\"" + f1(c.px(p->getX())) + "\" cy=\"" + f1(c.py(p->getY())) + "\" r=\"3.6\"/>";
            break;
        }
        default:
            if (OGR_GT_IsSubClassOf(wkbFlatten(g->getGeometryType()), wkbGeometryCollection))
                for (const auto* part : *g->toGeometryCollection()) geom_paths(part, c, poly, line, pts);
    }
}

std::string vector_svg_once(const Layer& layer, const std::string& title, double tolerance_px) {
    const Canvas c(layer.bounds(), srs_geographic(layer.srs));
    const double tol = c.world_per_px * tolerance_px;
    std::string poly, line, pts;
    for (const auto& g : layer.geoms) {
        if (!g || g->IsEmpty()) continue;
        const auto t = wkbFlatten(g->getGeometryType());
        if (tol > 0 && t != wkbPoint && t != wkbMultiPoint) {
            GeomPtr s = own(g->Simplify(tol));
            geom_paths(s ? s.get() : g.get(), c, poly, line, pts);
        } else {
            geom_paths(g.get(), c, poly, line, pts);
        }
    }
    std::string body;
    if (!poly.empty()) body += "<path class=\"poly\" fill-rule=\"evenodd\" d=\"" + poly + "\"/>";
    if (!line.empty()) body += "<path class=\"line\" d=\"" + line + "\"/>";
    if (!pts.empty()) body += "<g class=\"pt\">" + pts + "</g>";
    std::string under, over;
    decor(c, layer.srs, under, over);
    return card(c, title, under, body, over);
}

std::string empty_svg(const std::string& text) {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + std::to_string(WIDTH) + " " + std::to_string(MIN_H) +
           "\" width=\"" + std::to_string(WIDTH) + "\" height=\"" + std::to_string(MIN_H) + "\"><style>" + STYLE +
           "</style><rect class=\"bg\" width=\"" + std::to_string(WIDTH) + "\" height=\"" + std::to_string(MIN_H) + "\" rx=\"" +
           std::to_string(RADIUS) + "\"/><text class=\"tick\" x=\"50%\" y=\"50%\" text-anchor=\"middle\" font-size=\"14\">" +
           esc(text) + "</text></svg>";
}

// ---------------------------------------------------------------- PNG

struct DsCloser {
    void operator()(GDALDataset* ds) const {
        if (ds) GDALClose(GDALDataset::ToHandle(ds));
    }
};
using DsPtr = std::unique_ptr<GDALDataset, DsCloser>;

/// RGBA 字节 → PNG（经 /vsimem/）
std::vector<unsigned char> encode_png(const std::vector<unsigned char>& rgba, int w, int h) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    DsPtr ds(mem->Create("", w, h, 4, GDT_Byte, nullptr));
    for (int b = 0; b < 4; ++b) {
        std::vector<unsigned char> band(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < band.size(); ++i) band[i] = rgba[i * 4 + b];
        ds->GetRasterBand(b + 1)->RasterIO(GF_Write, 0, 0, w, h, band.data(), w, h, GDT_Byte, 0, 0, nullptr);
    }
    const std::string name = "/vsimem/claude_gis_" + random_token(8) + ".png";
    GDALDriver* png = GetGDALDriverManager()->GetDriverByName("PNG");
    if (!png) return {};
    DsPtr out(png->CreateCopy(name.c_str(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    out.reset();
    vsi_l_offset len = 0;
    GByte* buf = VSIGetMemFileBuffer(name.c_str(), &len, FALSE);
    std::vector<unsigned char> bytes(buf, buf + len);
    VSIUnlink(name.c_str());
    return bytes;
}

/// viridis 色带（9 个关键色线性插值）
void viridis(double t, unsigned char* rgb) {
    static const double stops[9][3] = {{68, 1, 84},    {71, 44, 122},   {59, 81, 139},   {44, 113, 142}, {33, 144, 141},
                                       {39, 173, 129}, {92, 200, 99},   {170, 220, 50},  {253, 231, 37}};
    t = std::clamp(t, 0.0, 1.0) * 8.0;
    const int i = std::min(static_cast<int>(t), 7);
    const double f = t - i;
    for (int k = 0; k < 3; ++k) rgb[k] = static_cast<unsigned char>(stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f);
}

}  // namespace

// ================================================================ 公开接口

Srs resolve_display_crs(const Srs& source, const std::string& request, const Bounds& bounds) {
    const std::string req = lower(trim(request));
    if (req.empty() || req == "source" || req == "源") return source;
    if (req == "utm") return source ? estimate_utm(bounds, source) : source;
    try {
        return srs_from_user(request, "crs");
    } catch (const GisError&) {
        throw GisError(E_BAD_REQUEST, "无法识别的坐标系：" + request,
                       json{{"hint", "可用 source / utm / EPSG:4326 / EPSG:4490 / EPSG:3857 等"}});
    }
}

Layer reproject_layer(const Layer& layer, const Srs& dst) {
    if (!dst || !layer.srs || srs_same(layer.srs, dst)) return layer;
    std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(layer.srs.get(), dst.get()));
    if (!ct) throw GisError(E_BAD_REQUEST, "无法建立坐标变换：" + srs_string(layer.srs) + " → " + srs_string(dst));
    Layer out = layer;
    for (auto& g : out.geoms) {
        if (!g) continue;
        GeomPtr moved = clone_geom(g.get());
        if (moved->transform(ct.get()) == OGRERR_NONE) g = moved;
    }
    out.srs = dst;
    return out;
}

std::string vector_svg(const Layer& layer, const std::string& title) {
    if (layer.empty() || !std::isfinite(layer.bounds()[0])) return empty_svg("空图层");
    for (double tol : {0.6, 1.5, 4.0, 10.0}) {
        std::string svg = vector_svg_once(layer, title, tol);
        if (svg.size() <= SVG_BUDGET) return svg;
    }
    // 要素太多：取代表点抽样
    Layer reps = layer.empty_like();
    const size_t step = std::max<size_t>(1, layer.size() / 2500);
    for (size_t i = 0; i < layer.size(); i += step) {
        if (!layer.geoms[i]) continue;
        OGRGeometryH h = OGR_G_PointOnSurface(OGRGeometry::ToHandle(layer.geoms[i].get()));
        if (h) {
            reps.push(layer.rows[i], own(OGRGeometry::FromHandle(h)));
        } else {
            auto* pt = new OGRPoint();
            layer.geoms[i]->Centroid(pt);
            reps.push(layer.rows[i], own(pt));
        }
    }
    return vector_svg_once(reps, title + "（抽样）", 0);
}

std::pair<std::string, std::vector<unsigned char>> raster_svg(const Raster& rf, const std::string& title) {
    std::vector<float> v = finite_values(rf.data);
    std::sort(v.begin(), v.end());
    const double lo = v.empty() ? 0 : quantile_sorted(v, 0.02), hi0 = v.empty() ? 1 : quantile_sorted(v, 0.98);
    const double hi = hi0 > lo ? hi0 : lo + 1;
    std::vector<unsigned char> png;
    std::string b64;
    for (int side = 420; side >= 64; side = static_cast<int>(side * 0.7)) {
        const int step = std::max(1, static_cast<int>(std::ceil(std::max(rf.width, rf.height) / double(side))));
        const int w = (rf.width + step - 1) / step, h = (rf.height + step - 1) / step;
        std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4, 0);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) {
                const float val = rf.at(std::min(r * step, rf.height - 1), std::min(c * step, rf.width - 1));
                unsigned char* px = &rgba[(static_cast<size_t>(r) * w + c) * 4];
                if (!std::isfinite(val)) continue;
                viridis((val - lo) / (hi - lo), px);
                px[3] = 255;
            }
        png = encode_png(rgba, w, h);
        b64 = base64(png);
        if (b64.size() < SVG_BUDGET - 6000) break;
    }
    const Canvas c(rf.bounds(), srs_geographic(rf.srs));
    std::string under, over;
    decor(c, rf.srs, under, over);
    const Bounds b = rf.bounds();
    const double x0 = c.px(b[0]), y0 = c.py(b[3]), x1 = c.px(b[2]), y1 = c.py(b[1]);
    const std::string body = "<image x=\"" + f1(x0) + "\" y=\"" + f1(y0) + "\" width=\"" + f1(x1 - x0) + "\" height=\"" +
                             f1(y1 - y0) + "\" preserveAspectRatio=\"none\" style=\"image-rendering:pixelated\" href=\"data:image/png;base64," +
                             b64 + "\"/>";
    return {card(c, title, under, body, over), png};
}

std::vector<unsigned char> vector_png(const Layer& layer, int width_px) {
    if (layer.empty()) return {};
    Bounds b = layer.bounds();
    const bool geographic = srs_geographic(layer.srs);
    const double kx = geographic ? std::max(std::cos((b[1] + b[3]) / 2 * PI / 180.0), 0.05) : 1.0;
    double w = std::max(b[2] - b[0], 1e-9), h = std::max(b[3] - b[1], 1e-9);
    b = {b[0] - w * 0.04, b[1] - h * 0.04, b[2] + w * 0.04, b[3] + h * 0.04};
    w = b[2] - b[0];
    h = b[3] - b[1];
    const int W = width_px;
    const int H = std::clamp(static_cast<int>(std::lround(W * h / (w * kx))), 120, W);
    const double sx = w / W, sy = h / H;
    const GeoTransform gt{b[0], sx, 0, b[3], 0, -sy};
    const double px_world = std::max(sx, sy);

    std::vector<GeomPtr> polys, lines, pts;
    for (const auto& g : layer.geoms) {
        if (!g) continue;
        const int dim = g->getDimension();
        if (dim == 2) polys.push_back(g);
        else if (dim == 1) lines.push_back(own(g->Buffer(px_world * 0.7, 4)));
        else pts.push_back(own(g->Buffer(px_world * 3.0, 6)));
    }
    std::vector<unsigned char> rgba(static_cast<size_t>(W) * H * 4, 0);
    auto paint = [&](const std::vector<GeomPtr>& geoms, const unsigned char rgb[3], unsigned char alpha) {
        if (geoms.empty()) return;
        const std::vector<float> m = rasterize_values(geoms, std::vector<double>(geoms.size(), 1.0), gt, H, W, true);
        for (size_t i = 0; i < m.size(); ++i) {
            if (!std::isfinite(m[i])) continue;
            unsigned char* p = &rgba[i * 4];
            for (int k = 0; k < 3; ++k) p[k] = rgb[k];
            p[3] = alpha;
        }
    };
    const unsigned char blue[3] = {59, 130, 246}, orange[3] = {249, 115, 22}, red[3] = {225, 29, 72};
    paint(polys, blue, 110);
    std::vector<GeomPtr> outlines;
    for (const auto& p : polys) outlines.push_back(own(p->Boundary()));
    for (auto& o : outlines) o = o ? own(o->Buffer(px_world * 0.5, 4)) : nullptr;
    paint(outlines, blue, 230);
    paint(lines, orange, 255);
    paint(pts, red, 255);
    return encode_png(rgba, W, H);
}

}  // namespace gis
