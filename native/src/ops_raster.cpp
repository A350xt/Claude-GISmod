// 栅格原语（raster 域）：一算子一语义、@步骤链式、统计摘要随产物、模板显式化。
//   基建    raster.info / create / from_vector / align / mask / merge
//   距离    raster.distance / distance_decay / cost_distance / cost_path / nearest
//   逐像元  raster.con / calc / reclassify / breaks / histogram / weighted_sum
//   邻域    raster.focal / zonal_stats
//   转出    raster.polygonize / contour / sample
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>

#include <gdal_alg.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "ops.hpp"

namespace gis {

namespace {

// ================================================================ 公共

std::shared_ptr<Raster> raster_param(Resolver& r, const json& tok) { return r.raster(to_str(tok)); }

std::shared_ptr<Layer> vector_param(Resolver& r, const json& tok, const std::string& label) {
    auto l = r.frame(to_str(tok));
    if (l->empty()) throw GisError(E_EMPTY_RESULT, label + "：输入为空");
    return l;
}

StepResult done(Raster rf, std::vector<std::string> summaries, json table = nullptr) {
    check_cells(rf);
    for (auto& s : rf.summaries()) summaries.push_back(s);
    return {nullptr, std::make_shared<Raster>(std::move(rf)), summaries, table};
}

Raster warn_aligned(Resolver& r, const std::string& name, const Raster& rf, const Raster& tmpl,
                    const std::string& method = "nearest") {
    if (same_grid(rf, tmpl)) return rf;
    r.warnings.push_back(name + " 与首输入模板不一致（origin/cellsize/形状），已自动重采样对齐（(" + std::to_string(rf.height) +
                         ", " + std::to_string(rf.width) + ")→(" + std::to_string(tmpl.height) + ", " +
                         std::to_string(tmpl.width) + "), cellsize " + py_float(rf.cellsize()) + "→" +
                         py_float(tmpl.cellsize()) + "）");
    return align_to(rf, tmpl, method);
}

/// 一个引用是不是栅格：@步骤 / ar_ 产物 / .tif 先看实际内容
bool looks_raster(Resolver& r, const std::string& tok) {
    try {
        return r.is_raster_token(tok);
    } catch (const GisError&) {
        return false;
    }
}

std::string num_text(const json& v) {
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return py_float(v.get<double>());
    return to_str(v);
}

Bounds padded_bounds(const Layer& l, double cellsize, double pad_m) {
    const Bounds b = l.bounds();
    const double pad = std::max(pad_m, 2.0 * cellsize);
    return {b[0] - pad, b[1] - pad, b[2] + pad, b[3] + pad};
}

double span_of(const Layer& l) {
    const Bounds b = l.bounds();
    return std::max(b[2] - b[0], b[3] - b[1]);
}

// ================================================================ 欧氏距离变换（Felzenszwalb）

/// 精确欧氏距离 + 最近源像元下标（像元中心、x/y 等间距）
struct Edt {
    std::vector<double> dist;  // 米；无源时 NaN
    std::vector<int> nearest;  // 最近源像元的线性下标；-1 = 无
};

void dt1d(const std::vector<double>& f, int n, std::vector<double>& d, std::vector<int>& arg) {
    std::vector<int> v(n);
    std::vector<double> z(n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -INFINITY;
    z[1] = INFINITY;
    for (int q = 1; q < n; ++q) {
        double s;
        while (true) {
            const int p = v[k];
            s = ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p);
            if (s <= z[k] && k > 0) {
                --k;
                continue;
            }
            break;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INFINITY;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
        arg[q] = v[k];
    }
}

Edt euclidean_distance(const std::vector<char>& source, int h, int w, double cellsize) {
    const double BIG = 1e20;
    std::vector<double> g(static_cast<size_t>(h) * w);
    std::vector<int> row_of(static_cast<size_t>(h) * w);
    {
        std::vector<double> f(h), d(h);
        std::vector<int> arg(h);
        for (int c = 0; c < w; ++c) {
            for (int r = 0; r < h; ++r) f[r] = source[static_cast<size_t>(r) * w + c] ? 0.0 : BIG;
            dt1d(f, h, d, arg);
            for (int r = 0; r < h; ++r) {
                g[static_cast<size_t>(r) * w + c] = d[r];
                row_of[static_cast<size_t>(r) * w + c] = arg[r];
            }
        }
    }
    Edt out;
    out.dist.assign(g.size(), NaN);
    out.nearest.assign(g.size(), -1);
    std::vector<double> f(w), d(w);
    std::vector<int> arg(w);
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) f[c] = g[static_cast<size_t>(r) * w + c];
        dt1d(f, w, d, arg);
        for (int c = 0; c < w; ++c) {
            if (d[c] >= BIG / 2) continue;
            const size_t i = static_cast<size_t>(r) * w + c;
            out.dist[i] = std::sqrt(d[c]) * cellsize;
            out.nearest[i] = row_of[static_cast<size_t>(r) * w + arg[c]] * w + arg[c];
        }
    }
    return out;
}

std::vector<char> finite_mask(const std::vector<float>& v) {
    std::vector<char> m(v.size());
    for (size_t i = 0; i < v.size(); ++i) m[i] = std::isfinite(v[i]) ? 1 : 0;
    return m;
}

std::vector<char> source_from_layer(const Layer& l, const Raster& tmpl) {
    auto mask = finite_mask(rasterize(l, tmpl.gt, tmpl.height, tmpl.width));
    if (std::none_of(mask.begin(), mask.end(), [](char c) { return c; }))
        throw GisError(E_EMPTY_RESULT, "源要素栅格化为空（可能在模板范围外）");
    return mask;
}

// ================================================================ 基建

StepResult handle_info(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.info"));
    std::vector<std::string> s = rf->summaries();
    s.push_back("元信息见 steps[].table；如需直方图/断点，接 raster.histogram / raster.breaks");
    return {nullptr, rf, s, rf->table()};
}

StepResult handle_create(Resolver& r, const json& p, const std::string&) {
    if (!p.contains("bounds") || !p["bounds"].is_array() || p["bounds"].size() != 4)
        throw GisError(E_BAD_REQUEST, "bounds 需要 [minx, miny, maxx, maxy]（分析坐标系的米制范围）");
    Bounds b;
    for (int i = 0; i < 4; ++i) b[i] = to_double(p["bounds"][i], "bounds");
    const double cellsize = to_double(need(p, "cellsize", "raster.create"), "cellsize");
    Srs crs;
    if (has(p, "crs") && !to_str(p["crs"]).empty()) crs = srs_from_user(to_str(p["crs"]), "crs");
    else if (r.analysis_crs()) crs = r.analysis_crs();
    else throw GisError(E_CRS_AMBIGUOUS, "create 需要 crs（或先在管线里引用一个输入以确定坐标系）");
    auto [gt, shape] = make_transform(b, cellsize);
    const bool has_value = has(p, "value");
    const float fill = has_value ? static_cast<float>(to_double(p["value"], "value")) : static_cast<float>(NaN);
    Raster rf = blank_raster(shape.first, shape.second, gt, crs, fill, opt_str(p, "name", "created"));
    return done(std::move(rf), {"创建空模板栅格：" + std::to_string(shape.second) + "×" + std::to_string(shape.first) +
                                "，cellsize=" + py_float(cellsize) + "，初值=" + (has_value ? num_text(p["value"]) : "NoData")});
}

StepResult handle_from_vector(Resolver& r, const json& p, const std::string&) {
    auto gdf = vector_param(r, need(p, "input", "raster.from_vector"), "from_vector");
    const std::string field = has(p, "field") ? to_str(p["field"]) : "";
    GeoTransform gt;
    int h, w;
    Srs crs;
    std::string note;
    if (has(p, "template")) {
        auto tmpl = raster_param(r, p["template"]);
        gt = tmpl->gt;
        h = tmpl->height;
        w = tmpl->width;
        crs = tmpl->srs;
        note = "对齐模板 " + to_str(p["template"]);
    } else {
        const double cellsize = to_double(need(p, "cellsize", "raster.from_vector"), "cellsize");
        auto [g, shape] = make_transform(gdf->bounds(), cellsize);
        gt = g;
        h = shape.first;
        w = shape.second;
        crs = gdf->srs;
        note = "cellsize=" + py_float(cellsize) + "（矢量外接矩形）";
    }
    Raster rf = blank_raster(h, w, gt, crs, static_cast<float>(NaN), field.empty() ? "mask" : field);
    rf.data = rasterize(*gdf, gt, h, w, field, 1.0, has(p, "all_touched") && to_bool(p["all_touched"]));
    if (rf.all_nodata())
        throw GisError(E_EMPTY_RESULT, "栅格化结果全空（矢量可能都在模板范围外）；如属预期请加 allow_empty");
    return done(std::move(rf), {"矢量栅格化：" + std::to_string(gdf->size()) + " 要素 → " + std::to_string(w) + "×" +
                                std::to_string(h) + " 像元，" + note + "，" +
                                (field.empty() ? std::string("覆盖处=1") : "取字段 " + field + " 的值")});
}

StepResult handle_align(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.align"));
    auto tmpl = raster_param(r, need(p, "template", "raster.align"));
    const std::string method = opt_str(p, "method", "nearest");
    Raster out = warn_aligned(r, "align.input", *rf, *tmpl, method);
    out.gt = tmpl->gt;
    out.srs = tmpl->srs;
    return done(std::move(out), {"对齐到模板（method=" + method + "）"});
}

StepResult handle_mask(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.mask"));
    const bool invert = has(p, "invert") && to_bool(p["invert"]);
    const std::string mask_tok = to_str(need(p, "mask", "raster.mask"));
    std::vector<char> keep;
    if (looks_raster(r, mask_tok)) {
        Raster m = warn_aligned(r, "mask", *raster_param(r, mask_tok), *rf);
        keep = finite_mask(m.data);
    } else {
        auto gdf = vector_param(r, mask_tok, "mask");
        keep = finite_mask(rasterize(*gdf, rf->gt, rf->height, rf->width));
    }
    if (invert)
        for (auto& k : keep) k = !k;
    Raster out = *rf;
    const bool has_fill = has(p, "fill");
    const float fill = has_fill ? static_cast<float>(to_double(p["fill"], "fill")) : static_cast<float>(NaN);
    size_t kept = 0;
    for (size_t i = 0; i < out.data.size(); ++i) {
        if (keep[i]) ++kept;
        else out.data[i] = fill;
    }
    if (kept == 0) throw GisError(E_EMPTY_RESULT, "掩膜后全空；如属预期请给该步加 allow_empty");
    return done(std::move(out), {"掩膜：保留 " + std::to_string(kept) + " / " + std::to_string(rf->cells()) + " 像元" +
                                 (invert ? "（反转）" : "") +
                                 (has_fill ? "，掩膜外填 " + num_text(p["fill"]) : std::string("，掩膜外置 NoData"))});
}

StepResult handle_merge(Resolver& r, const json& p, const std::string&) {
    if (!p.contains("inputs") || !p["inputs"].is_array() || p["inputs"].empty())
        throw GisError(E_BAD_REQUEST, "merge 需要 inputs（栅格引用数组）");
    std::vector<std::shared_ptr<Raster>> rfs;
    for (const auto& t : p["inputs"]) rfs.push_back(raster_param(r, t));
    Bounds u = rfs[0]->bounds();
    for (const auto& rf : rfs) {
        const Bounds b = rf->bounds();
        u = {std::min(u[0], b[0]), std::min(u[1], b[1]), std::max(u[2], b[2]), std::max(u[3], b[3])};
    }
    auto [gt, shape] = make_transform(u, rfs[0]->cellsize());
    Raster canvas = blank_raster(shape.first, shape.second, gt, rfs[0]->srs, static_cast<float>(NaN), rfs[0]->name);
    size_t placed = 0;
    for (const auto& rf : rfs) {
        Raster aligned = align_to(*rf, canvas, "nearest");
        for (size_t i = 0; i < canvas.data.size(); ++i) {
            if (std::isfinite(aligned.data[i]) && std::isnan(canvas.data[i])) {
                canvas.data[i] = aligned.data[i];
                ++placed;
            }
        }
    }
    if (placed == 0) throw GisError(E_EMPTY_RESULT, "镶嵌结果全空");
    return done(std::move(canvas), {"镶嵌 " + std::to_string(rfs.size()) + " 幅 → " + std::to_string(shape.second) + "×" +
                                    std::to_string(shape.first) + "，有效像元 " + std::to_string(placed) + "（重叠处先到先得）"});
}

// ================================================================ 距离

StepResult handle_distance(Resolver& r, const json& p, const std::string&) {
    auto gdf = vector_param(r, need(p, "input", "raster.distance"), "distance");
    double cellsize = has(p, "cellsize") ? to_double(p["cellsize"], "cellsize") : 0;
    if (!has(p, "cellsize")) {
        double span = span_of(*gdf);
        if (!(span > 0)) span = 1000.0;
        cellsize = std::max(span / 400.0, 1.0);
    }
    auto [gt, shape] = make_transform(padded_bounds(*gdf, cellsize, opt_double(p, "pad_m", 0)), cellsize);
    Raster tmpl = blank_raster(shape.first, shape.second, gt, gdf->srs, static_cast<float>(NaN), "distance_m");
    Edt e = euclidean_distance(source_from_layer(*gdf, tmpl), tmpl.height, tmpl.width, cellsize);
    const bool cut = has(p, "max_distance_m");
    const double maxd = cut ? to_double(p["max_distance_m"], "max_distance_m") : INFINITY;
    for (size_t i = 0; i < e.dist.size(); ++i)
        tmpl.data[i] = (std::isfinite(e.dist[i]) && e.dist[i] <= maxd) ? static_cast<float>(e.dist[i]) : static_cast<float>(NaN);
    return done(std::move(tmpl), {"欧氏距离场：源 " + std::to_string(gdf->size()) + " 要素（源处=0），单位米" +
                                  (cut ? "，>" + num_text(p["max_distance_m"]) + "m 截断为 NoData" : std::string())});
}

StepResult handle_distance_decay(Resolver& r, const json& p, const std::string&) {
    const double d0 = to_double(need(p, "d0", "raster.distance_decay"), "d0");
    if (!(d0 > 0)) throw GisError(E_BAD_REQUEST, "d0 必须为正数：" + py_float(d0));
    const double f0 = opt_double(p, "f0", 100.0);
    const std::string mode = opt_str(p, "mode", "linear");
    if (mode != "linear" && mode != "square" && mode != "exponential")
        throw GisError(E_BAD_REQUEST, "未知 mode：" + mode + "（可选 ('linear', 'square', 'exponential')）");
    auto gdf = vector_param(r, need(p, "input", "raster.distance_decay"), "decay");
    double cellsize;
    if (has(p, "cellsize")) {
        cellsize = to_double(p["cellsize"], "cellsize");
    } else {
        double span = span_of(*gdf);
        if (!(span > 0)) span = d0;
        cellsize = std::max({span / 400.0, d0 / 100.0, 1.0});
    }
    auto [gt, shape] = make_transform(padded_bounds(*gdf, cellsize, d0 * 0.05), cellsize);
    Raster out = blank_raster(shape.first, shape.second, gt, gdf->srs, static_cast<float>(NaN), "score");
    Edt e = euclidean_distance(source_from_layer(*gdf, out), out.height, out.width, cellsize);
    for (size_t i = 0; i < e.dist.size(); ++i) {
        const double d = e.dist[i];
        if (!std::isfinite(d)) continue;
        const double x = std::clamp(d / d0, 0.0, 1.0);
        double s = mode == "linear" ? f0 * (1 - x) : mode == "square" ? f0 * (1 - x * x) : f0 * std::exp(-x);
        if (d > d0) s = 0.0;
        out.data[i] = static_cast<float>(s);
    }
    return done(std::move(out), {"距离衰减作用分：d0=" + py_float(d0) + "m, f0=" + py_float(f0) + ", mode=" + mode +
                                 "；源处=" + py_float(f0) + "，>" + py_float(d0) + "m=0"});
}

/// Dijkstra 累积成本；cost 为 NaN 或 ≤0 处不可通行
void cost_dijkstra(const Raster& cost, const std::vector<char>& sources, std::vector<double>& dist, std::vector<int>& back) {
    const int h = cost.height, w = cost.width;
    const double cell = cost.cellsize();
    dist.assign(cost.cells(), INFINITY);
    back.assign(cost.cells(), -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    auto walkable = [&](int i) { return std::isfinite(cost.data[i]) && cost.data[i] > 0; };
    for (int i = 0; i < static_cast<int>(cost.cells()); ++i) {
        if (sources[i] && walkable(i)) {
            dist[i] = 0.0;
            pq.push({0.0, i});
        }
    }
    if (pq.empty()) throw GisError(E_EMPTY_RESULT, "成本距离：没有落在可通行区的源像元");
    static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1}, dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    while (!pq.empty()) {
        auto [d, i] = pq.top();
        pq.pop();
        if (d > dist[i]) continue;
        const int r0 = i / w, c0 = i % w;
        for (int k = 0; k < 8; ++k) {
            const int rr = r0 + dr[k], cc = c0 + dc[k];
            if (rr < 0 || rr >= h || cc < 0 || cc >= w) continue;
            const int j = rr * w + cc;
            if (!walkable(j)) continue;
            const double step = cost.data[j] * cell * ((dr[k] && dc[k]) ? std::sqrt(2.0) : 1.0);
            if (d + step < dist[j] - 1e-9) {
                dist[j] = d + step;
                back[j] = i;
                pq.push({dist[j], j});
            }
        }
    }
}

std::vector<char> source_mask(Resolver& r, const std::string& tok, const Raster& tmpl) {
    if (looks_raster(r, tok)) {
        Raster src = warn_aligned(r, "source", *raster_param(r, tok), tmpl);
        return finite_mask(src.data);
    }
    auto gdf = vector_param(r, tok, "source");
    return finite_mask(rasterize(*gdf, tmpl.gt, tmpl.height, tmpl.width));
}

StepResult handle_cost_distance(Resolver& r, const json& p, const std::string&) {
    auto cost = raster_param(r, need(p, "cost", "raster.cost_distance"));
    auto src = source_mask(r, to_str(need(p, "source", "raster.cost_distance")), *cost);
    std::vector<double> dist;
    std::vector<int> back;
    cost_dijkstra(*cost, src, dist, back);
    Raster out = *cost;
    out.name = "cost_dist";
    size_t valid = 0;
    for (size_t i = 0; i < dist.size(); ++i) {
        out.data[i] = std::isfinite(dist[i]) ? static_cast<float>(dist[i]) : static_cast<float>(NaN);
        valid += std::isfinite(dist[i]);
    }
    if (!valid) throw GisError(E_EMPTY_RESULT, "成本距离全空（源与成本面不连通？）");
    return done(std::move(out), {"累积成本距离：单位 = cost值×米；NoData/≤0 成本视为不可通行"});
}

StepResult handle_cost_path(Resolver& r, const json& p, const std::string&) {
    auto cost = raster_param(r, need(p, "cost", "raster.cost_path"));
    auto src = source_mask(r, to_str(need(p, "source", "raster.cost_path")), *cost);
    auto to = vector_param(r, need(p, "to", "raster.cost_path"), "cost_path.to");
    if (to->size() > 1) r.warnings.push_back("cost_path.to 有多个点，取第一个");
    std::vector<double> dist;
    std::vector<int> back;
    cost_dijkstra(*cost, src, dist, back);
    OGRPoint target;
    to->geoms[0]->Centroid(&target);
    auto [colf, rowf] = cost->to_pixel(target.getX(), target.getY());
    // 与 Python 版一致：仿射逆变换后四舍五入取像元
    const int rr = static_cast<int>(std::lround(rowf)), cc = static_cast<int>(std::lround(colf));
    if (rr < 0 || rr >= cost->height || cc < 0 || cc >= cost->width) throw GisError(E_BAD_REQUEST, "目标点在成本栅格范围外");
    int i = rr * cost->width + cc;
    if (!std::isfinite(dist[i])) throw GisError(E_EMPTY_RESULT, "目标不可达（与源不连通或被 NoData/零成本阻断）");
    const double total = dist[i];
    auto* line = new OGRLineString();
    size_t n = 0;
    for (; i != -1; i = back[i], ++n) {
        auto [x, y] = cost->center_of(i / cost->width, i % cost->width);
        line->addPoint(x, y);
    }
    if (line->getNumPoints() == 1) line->addPoint(line->getX(0), line->getY(0));
    Layer out;
    out.srs = cost->srs;
    out.fields = {{"total_cost", FieldType::Real}, {"n_cells", FieldType::Integer}};
    out.push({total, static_cast<std::int64_t>(n)}, own(line));
    return {std::make_shared<Layer>(std::move(out)), nullptr,
            {"成本最短路径：" + std::to_string(n) + " 像元，总成本 " + fmt_fixed(total, 3)}, nullptr};
}

StepResult handle_nearest(Resolver& r, const json& p, const std::string&) {
    auto gdf = vector_param(r, need(p, "input", "raster.nearest"), "nearest");
    auto ref = vector_param(r, need(p, "ref", "raster.nearest"), "nearest.ref");
    double cellsize;
    if (has(p, "cellsize")) {
        cellsize = to_double(p["cellsize"], "cellsize");
    } else {
        double span = span_of(*gdf);
        if (!(span > 0)) span = 1000.0;
        cellsize = span / 300.0;
    }
    auto [gt, shape] = make_transform(padded_bounds(*gdf, cellsize, 0), cellsize);
    std::vector<double> ids(ref->size());
    for (size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<double>(i + 1);
    const std::vector<float> idgrid = rasterize_values(ref->geoms, ids, gt, shape.first, shape.second);
    const auto src = finite_mask(idgrid);
    if (std::none_of(src.begin(), src.end(), [](char c) { return c; })) throw GisError(E_EMPTY_RESULT, "参照层栅格化为空");
    Edt e = euclidean_distance(src, shape.first, shape.second, cellsize);
    Raster out = blank_raster(shape.first, shape.second, gt, gdf->srs, static_cast<float>(NaN), "nearest_id");
    double far = 0;
    for (size_t i = 0; i < out.data.size(); ++i) {
        if (e.nearest[i] < 0) continue;
        out.data[i] = idgrid[e.nearest[i]];
        far = std::max(far, e.dist[i]);
    }
    return done(std::move(out), {"就近分配：每个像元 = 最近参照要素的序号（1.." + std::to_string(ref->size()) +
                                 "），最大最近距离 " + fmt_fixed(far, 1) + " m；距离场请用 raster.distance {input, ref: 同参照层}"});
}

// ================================================================ 逐像元

struct Condition {
    bool nodata = false;
    std::string op;
    double threshold = 0;
    bool test(double v) const {
        if (op == "<=") return v <= threshold;
        if (op == ">=") return v >= threshold;
        if (op == "==") return v == threshold;
        if (op == "!=") return v != threshold;
        if (op == "<") return v < threshold;
        return v > threshold;
    }
};

Condition parse_condition(const std::string& expr) {
    const std::string e = trim(expr);
    if (lower(e) == "nodata" || lower(e) == "isnan") return {true, "", 0};
    for (const char* op : {"<=", ">=", "==", "!=", "<", ">"}) {
        const size_t pos = e.find(op);
        if (pos == std::string::npos) continue;
        const std::string right = trim(e.substr(pos + std::char_traits<char>::length(op)));
        char* end = nullptr;
        const double thr = std::strtod(right.c_str(), &end);
        if (right.empty() || end != right.c_str() + right.size()) {
            throw GisError(E_BAD_REQUEST, "condition 右侧需为数字：'" + expr + "'",
                           json{{"grammar", "值 <op> 数字，如 'value >= 500' 或 'nodata'；左标识符随意（指代像元值）"}});
        }
        return {false, op, thr};
    }
    throw GisError(E_BAD_REQUEST, "无法解析 condition：'" + expr + "'", json{{"grammar", "'value >= 500' / 'dist < d0' / 'nodata'"}});
}

StepResult handle_con(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.con"));
    const Condition cond = parse_condition(to_str(need(p, "condition", "raster.con")));
    const json t = p.value("true", json(nullptr)), f = p.value("false", json(nullptr));
    auto apply = [](const json& v, float& cell) {
        if (v.is_string() && lower(trim(v.get<std::string>())) == "nodata") cell = static_cast<float>(NaN);
        else if (!v.is_null()) cell = static_cast<float>(to_double(v, "true/false"));
    };
    Raster out = *rf;
    size_t n_true = 0;
    for (size_t i = 0; i < out.data.size(); ++i) {
        const float v = rf->data[i];
        // 比较条件不改动 NoData 像元；处理 NoData 用 condition='nodata'
        const bool is_true = cond.nodata ? std::isnan(v) : (std::isfinite(v) && cond.test(v));
        const bool is_false = cond.nodata ? !std::isnan(v) : (std::isfinite(v) && !cond.test(v));
        if (is_true) {
            ++n_true;
            apply(t, out.data[i]);
        } else if (is_false) {
            apply(f, out.data[i]);
        }
    }
    return done(std::move(out), {"条件赋值：" + std::to_string(n_true) + " / " + std::to_string(rf->cells()) + " 像元为真 → true=" +
                                 repr(t) + "，false=" + repr(f) + "（比较条件不改动 NoData 像元；处理 NoData 用 condition='nodata'）"});
}

// ---------------------------------------------------------------- calc 表达式

class CalcExpr {
public:
    CalcExpr(const std::string& src, const std::vector<std::string>& names) : src_(src), names_(names) {
        lex();
        root_ = parse_add();
        if (pos_ < toks_.size()) bad();
    }
    double eval(const std::vector<double>& vars) const { return root_(vars); }

private:
    using Fn = std::function<double(const std::vector<double>&)>;
    std::string src_;
    std::vector<std::string> names_;
    std::vector<std::string> toks_;
    size_t pos_ = 0;
    Fn root_;

    [[noreturn]] void bad() const {
        throw GisError(E_BAD_REQUEST, "表达式含不允许的语法：'" + src_ + "'",
                       json{{"allowed", "四则/乘方/取模 + abs/min/max/round/clip/sqrt/exp/ln(别名…)"}});
    }
    void lex() {
        size_t i = 0;
        while (i < src_.size()) {
            const unsigned char c = src_[i];
            if (std::isspace(c)) {
                ++i;
            } else if (std::isdigit(c) || c == '.') {
                size_t j = i;
                while (j < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[j])) || src_[j] == '.')) ++j;
                if (j < src_.size() && (src_[j] == 'e' || src_[j] == 'E')) {
                    ++j;
                    if (j < src_.size() && (src_[j] == '+' || src_[j] == '-')) ++j;
                    while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j]))) ++j;
                }
                toks_.push_back(src_.substr(i, j - i));
                i = j;
            } else if (std::isalpha(c) || c == '_' || c >= 0x80) {
                size_t j = i;
                while (j < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[j])) || src_[j] == '_' ||
                                           static_cast<unsigned char>(src_[j]) >= 0x80))
                    ++j;
                toks_.push_back(src_.substr(i, j - i));
                i = j;
            } else if (src_.compare(i, 2, "**") == 0) {
                toks_.push_back("**");
                i += 2;
            } else if (std::string("+-*/%(),").find(static_cast<char>(c)) != std::string::npos) {
                toks_.push_back(std::string(1, static_cast<char>(c)));
                ++i;
            } else {
                bad();
            }
        }
    }
    bool at(const char* t) const { return pos_ < toks_.size() && toks_[pos_] == t; }
    Fn parse_add() {
        Fn l = parse_mul();
        while (at("+") || at("-")) {
            const bool plus = toks_[pos_++] == "+";
            Fn rr = parse_mul();
            l = [l, rr, plus](const auto& v) { return plus ? l(v) + rr(v) : l(v) - rr(v); };
        }
        return l;
    }
    Fn parse_mul() {
        Fn l = parse_unary();
        while (at("*") || at("/") || at("%")) {
            const std::string op = toks_[pos_++];
            Fn rr = parse_unary();
            if (op == "*") l = [l, rr](const auto& v) { return l(v) * rr(v); };
            else if (op == "/") l = [l, rr](const auto& v) { return l(v) / rr(v); };
            else
                l = [l, rr](const auto& v) {
                    const double a = l(v), b = rr(v);
                    return a - std::floor(a / b) * b;
                };
        }
        return l;
    }
    Fn parse_unary() {
        if (at("-")) {
            ++pos_;
            Fn x = parse_unary();
            return [x](const auto& v) { return -x(v); };
        }
        if (at("+")) {
            ++pos_;
            return parse_unary();
        }
        return parse_pow();
    }
    Fn parse_pow() {
        Fn base = parse_atom();
        if (at("**")) {
            ++pos_;
            Fn e = parse_unary();
            return [base, e](const auto& v) { return std::pow(base(v), e(v)); };
        }
        return base;
    }
    Fn parse_atom() {
        if (pos_ >= toks_.size()) bad();
        const std::string t = toks_[pos_++];
        if (t == "(") {
            Fn x = parse_add();
            if (!at(")")) bad();
            ++pos_;
            return x;
        }
        if (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.') {
            char* end = nullptr;
            const double c = std::strtod(t.c_str(), &end);
            if (end != t.c_str() + t.size()) bad();
            return [c](const auto&) { return c; };
        }
        if (at("(")) {  // 函数调用
            ++pos_;
            std::vector<Fn> args;
            if (!at(")")) {
                args.push_back(parse_add());
                while (at(",")) {
                    ++pos_;
                    args.push_back(parse_add());
                }
            }
            if (!at(")")) bad();
            ++pos_;
            return make_call(t, args);
        }
        const auto it = std::find(names_.begin(), names_.end(), t);
        if (it == names_.end()) {
            std::vector<std::string> declared = names_;
            std::sort(declared.begin(), declared.end());
            throw GisError(E_BAD_REQUEST, "表达式中引用了未声明的别名：" + t, json{{"declared", declared}});
        }
        const size_t idx = static_cast<size_t>(it - names_.begin());
        return [idx](const auto& v) { return v[idx]; };
    }
    Fn make_call(const std::string& name, std::vector<Fn> a) {
        auto arity = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) bad();
        };
        if (name == "abs") { arity(1, 1); return [a](const auto& v) { return std::fabs(a[0](v)); }; }
        if (name == "sqrt") { arity(1, 1); return [a](const auto& v) { return std::sqrt(a[0](v)); }; }
        if (name == "exp") { arity(1, 1); return [a](const auto& v) { return std::exp(a[0](v)); }; }
        if (name == "ln") { arity(1, 1); return [a](const auto& v) { return std::log(a[0](v)); }; }
        if (name == "min") { arity(2, 2); return [a](const auto& v) { const double x = a[0](v), y = a[1](v); return std::isnan(x) || std::isnan(y) ? NaN : std::min(x, y); }; }
        if (name == "max") { arity(2, 2); return [a](const auto& v) { const double x = a[0](v), y = a[1](v); return std::isnan(x) || std::isnan(y) ? NaN : std::max(x, y); }; }
        if (name == "clip") { arity(3, 3); return [a](const auto& v) { return std::clamp(a[0](v), a[1](v), a[2](v)); }; }
        if (name == "round") {
            arity(1, 2);
            return [a](const auto& v) {
                const double n = a.size() > 1 ? a[1](v) : 0;
                return round_to(a[0](v), static_cast<int>(n));
            };
        }
        bad();
    }
};

StepResult handle_calc(Resolver& r, const json& p, const std::string&) {
    const std::string expr = to_str(need(p, "expression", "raster.calc"));
    if (!p.contains("inputs") || !p["inputs"].is_object() || p["inputs"].empty())
        throw GisError(E_BAD_REQUEST, "calc 需要 inputs：{别名: 栅格引用}，表达式中用别名引用");
    if (p["inputs"].size() > 8) throw GisError(E_BAD_REQUEST, "calc 最多 8 个输入栅格");
    std::vector<std::string> names;
    std::vector<Raster> layers;
    std::shared_ptr<Raster> first;
    for (auto it = p["inputs"].begin(); it != p["inputs"].end(); ++it) {
        auto rf = raster_param(r, it.value());
        if (!first) first = rf;
        names.push_back(it.key());
        layers.push_back(warn_aligned(r, "calc." + it.key(), *rf, *first));
    }
    const CalcExpr ce(expr, names);
    Raster out = blank_raster(first->height, first->width, first->gt, first->srs, static_cast<float>(NaN), "calc");
    std::vector<double> vars(names.size());
    for (size_t i = 0; i < out.data.size(); ++i) {
        for (size_t k = 0; k < layers.size(); ++k) vars[k] = layers[k].data[i];
        const double v = ce.eval(vars);
        out.data[i] = std::isfinite(v) ? static_cast<float>(v) : static_cast<float>(NaN);
    }
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    return done(std::move(out), {"逐像元计算：" + expr + "（输入 " + py_list(sorted) + "）"});
}

StepResult handle_reclassify(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.reclassify"));
    const json mapping = p.value("mapping", json(nullptr));
    bool ok = mapping.is_array() && !mapping.empty();
    if (ok)
        for (const auto& m : mapping) ok = ok && m.is_array() && m.size() == 3;
    if (!ok) throw GisError(E_BAD_REQUEST, "mapping 需要 [[lo, hi, new], …] 行表（左闭右开）", json{{"example", "[[0, 200, 1], [200, 400, 2]]"}});
    Raster out = blank_raster(rf->height, rf->width, rf->gt, rf->srs, static_cast<float>(NaN), "class");
    size_t mapped = 0;
    std::set<double> classes;
    for (const auto& m : mapping) {
        const double lo = to_double(m[0], "lo"), hi = to_double(m[1], "hi"), nv = to_double(m[2], "new");
        classes.insert(nv);
        for (size_t i = 0; i < out.data.size(); ++i) {
            const float v = rf->data[i];
            if (v >= lo && v < hi) {
                out.data[i] = static_cast<float>(nv);
                ++mapped;
            }
        }
    }
    if (has(p, "nodata")) {
        const float nd = static_cast<float>(to_double(p["nodata"], "nodata"));
        for (size_t i = 0; i < out.data.size(); ++i)
            if (std::isnan(rf->data[i])) out.data[i] = nd;
    }
    if (mapped == 0) throw GisError(E_EMPTY_RESULT, "重分类没有任何像元落表；检查区间与数值范围（可用 raster.breaks 先看断点）");
    return done(std::move(out), {"重分类：" + std::to_string(mapping.size()) + " 条规则，命中 " + std::to_string(mapped) +
                                 " 像元，级别 " + py_list(std::vector<double>(classes.begin(), classes.end())) +
                                 "；未落表且未给 nodata 的像元 = NoData"});
}

/// Fisher-Jenks 自然断点（去重后抽样 ≤ 2000 控制规模）
std::vector<double> jenks(std::vector<float> values, int k) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::vector<double> v(values.begin(), values.end());
    if (v.size() > 2000) {
        std::vector<double> s;
        for (int i = 0; i < 2000; ++i) s.push_back(v[static_cast<size_t>(std::floor(i * (v.size() - 1) / 1999.0))]);
        v = s;
    }
    const int n = static_cast<int>(v.size());
    k = std::min(k, n - 1);
    std::vector<std::vector<int>> mat1(n + 1, std::vector<int>(k + 1, 0));
    std::vector<std::vector<double>> mat2(n + 1, std::vector<double>(k + 1, INFINITY));
    for (int j = 1; j <= k; ++j) {
        mat1[1][j] = 1;
        mat2[1][j] = 0.0;
    }
    for (int l = 2; l <= n; ++l) {
        double s1 = 0, s2 = 0, w = 0;
        for (int m = 1; m <= l; ++m) {
            const int i3 = l - m + 1;
            const double val = v[i3 - 1];
            s2 += val * val;
            s1 += val;
            w += 1;
            if (w == 1) continue;
            const double sd = s2 - s1 * s1 / w;
            for (int j = 2; j <= k; ++j) {
                if (mat2[l][j] >= sd + mat2[i3 - 1][j - 1]) {
                    mat2[l][j] = sd + mat2[i3 - 1][j - 1];
                    mat1[l][j] = i3 - 1;
                }
            }
            if (mat2[l][1] > sd) {
                mat2[l][1] = sd;
                mat1[l][1] = l;
            }
        }
    }
    std::vector<double> kclass(k + 1, 0.0);
    kclass[k] = v[n - 1];
    kclass[0] = v[0];
    int l = n;
    for (int j = k; j > 1; --j) {
        const int idx = mat1[l][j] - 2;
        if (idx < 0) break;
        kclass[j - 1] = v[idx];
        l = mat1[l][j] - 1;
        if (l < 2) break;
    }
    return kclass;
}

StepResult handle_breaks(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.breaks"));
    const int k = opt_int(p, "classes", 5);
    if (k < 2 || k > 20) throw GisError(E_BAD_REQUEST, "classes 需在 2..20：" + std::to_string(k));
    const std::string method = opt_str(p, "method", "quantile");
    std::vector<float> v = finite_values(rf->data);
    if (v.size() < static_cast<size_t>(k))
        throw GisError(E_EMPTY_RESULT, "有效像元 " + std::to_string(v.size()) + " 少于 classes " + std::to_string(k));
    std::sort(v.begin(), v.end());
    std::vector<double> br;
    if (method == "equal_interval") {
        for (int i = 0; i <= k; ++i) br.push_back(v.front() + (double(v.back()) - v.front()) * i / k);
    } else if (method == "quantile") {
        for (int i = 0; i <= k; ++i) br.push_back(quantile_sorted(v, double(i) / k));
    } else if (method == "jenks") {
        br = jenks(v, k);
    } else {
        throw GisError(E_BAD_REQUEST, "未知 method：" + method + "（可选 equal_interval/quantile/jenks）");
    }
    std::set<double> uniq;
    for (double b : br) uniq.insert(round_to(b, 6));
    std::vector<double> b(uniq.begin(), uniq.end());
    json rows = json::array();
    for (size_t i = 0; i + 1 < b.size(); ++i) rows.push_back({i, b[i], b[i + 1]});
    json table = {{"columns", {"idx", "lo", "hi"}}, {"rows", rows}};
    Raster same = *rf;
    return done(std::move(same),
                {"断点（" + method + "，" + std::to_string(b.size() - 1) +
                 " 类）：只求断点不执行；下一步接 raster.reclassify mapping=[[lo,hi,new]…]"},
                table);
}

StepResult handle_histogram(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.histogram"));
    const int bins = opt_int(p, "bins", 20);
    if (bins < 2 || bins > 200) throw GisError(E_BAD_REQUEST, "bins 需在 2..200：" + std::to_string(bins));
    std::vector<float> v = finite_values(rf->data);
    double lo = 0, hi = 1;
    if (!v.empty()) {
        lo = *std::min_element(v.begin(), v.end());
        hi = *std::max_element(v.begin(), v.end());
    }
    if (lo == hi) {  // 与 numpy.histogram 一致
        lo -= 0.5;
        hi += 0.5;
    }
    std::vector<long long> counts(bins, 0);
    const double width = (hi - lo) / bins;
    for (float x : v) {
        int i = static_cast<int>((x - lo) / width);
        if (i >= bins) i = bins - 1;
        if (i < 0) i = 0;
        ++counts[i];
    }
    const long long total = static_cast<long long>(v.size());
    long long cum = 0;
    json rows = json::array();
    for (int i = 0; i < bins; ++i) {
        cum += counts[i];
        rows.push_back({round_to(lo + width * i, 6), round_to(lo + width * (i + 1), 6), counts[i],
                        total ? round_to(cum * 100.0 / total, 2) : 0.0});
    }
    json table = {{"columns", {"lo", "hi", "count", "cumulative_pct"}}, {"rows", rows}};
    Raster same = *rf;
    return done(std::move(same),
                {"直方图：" + std::to_string(bins) + " 桶，有效像元 " + std::to_string(total) + "（累计频率可直接用于频率曲线定级）"},
                table);
}

StepResult handle_weighted_sum(Resolver& r, const json& p, const std::string&) {
    const json items = p.value("inputs", json(nullptr));
    bool ok = items.is_array() && !items.empty();
    if (ok)
        for (const auto& x : items) ok = ok && x.is_object() && x.contains("ref");
    if (!ok) throw GisError(E_BAD_REQUEST, "inputs 需要 [{ref, weight}, …]（weight 可省略=1）");
    std::vector<double> weights;
    for (const auto& x : items) weights.push_back(x.contains("weight") ? to_double(x["weight"], "weight") : 1.0);
    if (std::any_of(weights.begin(), weights.end(), [](double w) { return w < 0; }))
        throw GisError(E_BAD_REQUEST, "weight 不能为负");
    double total_w = 0;
    for (double w : weights) total_w += w;
    if (total_w <= 0) throw GisError(E_BAD_REQUEST, "weight 全为 0，无法归一化");
    auto first = raster_param(r, items[0]["ref"]);
    std::vector<double> acc(first->cells(), 0.0);
    std::vector<char> hit(first->cells(), 0);
    for (size_t k = 0; k < items.size(); ++k) {
        Raster d = warn_aligned(r, "weighted_sum." + to_str(items[k]["ref"]), *raster_param(r, items[k]["ref"]), *first);
        for (size_t i = 0; i < acc.size(); ++i) {
            if (!std::isfinite(d.data[i])) continue;
            acc[i] += d.data[i] * weights[k];
            hit[i] = 1;
        }
    }
    Raster out = blank_raster(first->height, first->width, first->gt, first->srs, static_cast<float>(NaN), "weighted");
    for (size_t i = 0; i < acc.size(); ++i)
        if (hit[i]) out.data[i] = static_cast<float>(acc[i] / total_w);
    std::vector<double> wr;
    for (double w : weights) wr.push_back(round_to(w, 4));
    return done(std::move(out), {"加权叠加：" + std::to_string(items.size()) + " 层，权重 " + py_list(wr) + " （自动归一化，和=" +
                                 py_float(round_to(total_w, 4)) + "）；某像元任一层 NoData 则不计该层"});
}

// ================================================================ 邻域

StepResult handle_focal(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.focal"));
    const std::string stat = lower(opt_str(p, "stat", "mean"));
    if (stat != "mean" && stat != "min" && stat != "max" && stat != "median" && stat != "std")
        throw GisError(E_BAD_REQUEST, "未知 stat：" + stat + "（可选 ['max', 'mean', 'median', 'min', 'std']）");
    const int k = opt_int(p, "kernel_size", 3);
    if (k < 3 || k % 2 == 0 || k > 15) throw GisError(E_BAD_REQUEST, "kernel_size 需为 3..15 的奇数：" + std::to_string(k));
    const int pad = k / 2, h = rf->height, w = rf->width;
    Raster out = blank_raster(h, w, rf->gt, rf->srs, static_cast<float>(NaN), "focal_" + stat);
    std::vector<double> win;
    win.reserve(k * k);
    for (int rr = 0; rr < h; ++rr) {
        for (int cc = 0; cc < w; ++cc) {
            win.clear();
            for (int dr = -pad; dr <= pad; ++dr)
                for (int dc = -pad; dc <= pad; ++dc) {
                    const int y = rr + dr, x = cc + dc;
                    if (y < 0 || y >= h || x < 0 || x >= w) continue;
                    const float v = rf->at(y, x);
                    if (std::isfinite(v)) win.push_back(v);
                }
            if (win.empty()) continue;
            double res;
            if (stat == "mean" || stat == "std") {
                double s = 0;
                for (double v : win) s += v;
                const double mean = s / win.size();
                if (stat == "mean") {
                    res = mean;
                } else {
                    double ss = 0;
                    for (double v : win) ss += (v - mean) * (v - mean);
                    res = std::sqrt(ss / win.size());
                }
            } else if (stat == "min") {
                res = *std::min_element(win.begin(), win.end());
            } else if (stat == "max") {
                res = *std::max_element(win.begin(), win.end());
            } else {
                std::sort(win.begin(), win.end());
                const size_t n = win.size();
                res = n % 2 ? win[n / 2] : (win[n / 2 - 1] + win[n / 2]) / 2.0;
            }
            out.at(rr, cc) = static_cast<float>(res);
        }
    }
    return done(std::move(out), {"邻域统计：" + std::to_string(k) + "×" + std::to_string(k) + " " + stat + "（边界像元按可用邻居计算）"});
}

StepResult handle_zonal_stats(Resolver& r, const json& p, const std::string&) {
    auto regions = vector_param(r, need(p, "regions", "raster.zonal_stats"), "zonal");
    auto rf = raster_param(r, need(p, "data", "raster.zonal_stats"));
    if (regions->family() != Family::Polygon) throw GisError(E_BAD_REQUEST, "regions 必须是面要素");
    const auto stats = opt_str_list(p, "stats", {"mean", "min", "max"});
    const std::set<std::string> known = {"count", "mean", "min", "max", "sum", "majority"};
    std::vector<std::string> unknown;
    for (const auto& s : stats)
        if (!known.count(s)) unknown.push_back(s);
    if (!unknown.empty()) throw GisError(E_BAD_REQUEST, "未知统计量：" + py_list(unknown), json{{"known", known}});

    auto out = std::make_shared<Layer>(*regions);
    std::vector<int> cols;
    for (const auto& s : stats) cols.push_back(out->add_field("raster_" + s, s == "count" ? FieldType::Integer : FieldType::Real));
    size_t covered = 0;
    for (size_t i = 0; i < out->size(); ++i) {
        const std::vector<float> m = rasterize_values({out->geoms[i]}, {1.0}, rf->gt, rf->height, rf->width, true);
        std::vector<double> vals;
        for (size_t c = 0; c < m.size(); ++c)
            if (std::isfinite(m[c]) && std::isfinite(rf->data[c])) vals.push_back(rf->data[c]);
        if (!vals.empty()) ++covered;
        for (size_t s = 0; s < stats.size(); ++s) {
            Value v = NaN;
            if (!vals.empty()) {
                const std::string& st = stats[s];
                if (st == "count") v = static_cast<std::int64_t>(vals.size());
                else if (st == "mean") {
                    double sum = 0;
                    for (double x : vals) sum += x;
                    v = sum / vals.size();
                } else if (st == "min") v = *std::min_element(vals.begin(), vals.end());
                else if (st == "max") v = *std::max_element(vals.begin(), vals.end());
                else if (st == "sum") {
                    double sum = 0;
                    for (double x : vals) sum += x;
                    v = sum;
                } else {  // majority：值取 6 位小数后的众数，并列取最小
                    std::map<double, size_t> cnt;
                    for (double x : vals) ++cnt[round_to(x, 6)];
                    double best = 0;
                    size_t bc = 0;
                    for (const auto& [val, c] : cnt)
                        if (c > bc) {
                            best = val;
                            bc = c;
                        }
                    v = best;
                }
            }
            out->rows[i][cols[s]] = v;
        }
    }
    std::vector<std::string> summaries = {
        "栅格分区统计：" + std::to_string(out->size()) + " 个区域，" + std::to_string(covered) + " 个含有效像元" +
            (covered < out->size() ? "（" + std::to_string(out->size() - covered) + " 个区域与栅格无有效交集）" : std::string()),
        "统计量 " + py_list(stats) + "，输出列前缀 raster_（数值列为各区域值）"};
    return {out, nullptr, summaries, nullptr};
}

// ================================================================ 转出（栅格→矢量）

struct DsCloser {
    void operator()(GDALDataset* ds) const {
        if (ds) GDALClose(GDALDataset::ToHandle(ds));
    }
};
using DsPtr = std::unique_ptr<GDALDataset, DsCloser>;

DsPtr mem_raster(const Raster& rf, const std::vector<float>& data) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    DsPtr ds(mem->Create("", rf.width, rf.height, 1, GDT_Float32, nullptr));
    GeoTransform gt = rf.gt;
    ds->SetGeoTransform(gt.data());
    if (rf.srs) ds->SetSpatialRef(rf.srs.get());
    ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, rf.width, rf.height, const_cast<float*>(data.data()), rf.width, rf.height,
                                   GDT_Float32, 0, 0, nullptr);
    return ds;
}

DsPtr mem_vector() {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!drv || !drv->GetMetadataItem(GDAL_DCAP_VECTOR)) drv = GetGDALDriverManager()->GetDriverByName("Memory");
    return DsPtr(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
}

StepResult handle_polygonize(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.polygonize"));
    const std::string field = opt_str(p, "field", "value");
    const bool dissolve = !p.contains("dissolve") || to_bool(p["dissolve"]);
    DsPtr src = mem_raster(*rf, rf->data);
    std::vector<float> maskv(rf->cells());
    for (size_t i = 0; i < maskv.size(); ++i) maskv[i] = std::isfinite(rf->data[i]) ? 1.0f : 0.0f;
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    DsPtr mask(mem->Create("", rf->width, rf->height, 1, GDT_Byte, nullptr));
    std::vector<unsigned char> mb(maskv.begin(), maskv.end());
    mask->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, rf->width, rf->height, mb.data(), rf->width, rf->height, GDT_Byte, 0, 0, nullptr);

    DsPtr vds = mem_vector();
    OGRLayer* lyr = vds->CreateLayer("polys", rf->srs.get(), wkbPolygon, nullptr);
    OGRFieldDefn fd("value", OFTReal);
    lyr->CreateField(&fd);
    if (GDALFPolygonize(GDALRasterBand::ToHandle(src->GetRasterBand(1)), GDALRasterBand::ToHandle(mask->GetRasterBand(1)), OGRLayer::ToHandle(lyr), 0, nullptr, nullptr, nullptr) != CE_None)
        throw GisError(E_BAD_REQUEST, std::string("栅格转面失败：") + CPLGetLastErrorMsg());

    std::map<double, std::vector<GeomPtr>> by_value;
    std::vector<std::pair<double, GeomPtr>> pieces;
    lyr->ResetReading();
    for (auto& feat : *lyr) {
        const double v = feat->GetFieldAsDouble(0);
        GeomPtr g = clone_geom(feat->GetGeometryRef());
        if (!g) continue;
        by_value[v].push_back(g);
        pieces.emplace_back(v, g);
    }
    if (pieces.empty()) throw GisError(E_EMPTY_RESULT, "没有可转换的有效像元");
    Layer out;
    out.srs = rf->srs;
    out.fields = {{field, FieldType::Real}};
    if (dissolve) {
        for (const auto& [v, geoms] : by_value) out.push({v}, unary_union(geoms));
    } else {
        for (const auto& [v, g] : pieces) out.push({v}, g);
    }
    const size_t n = out.size();
    return {std::make_shared<Layer>(std::move(out)), nullptr,
            {"栅格转面：" + std::to_string(n) + " 个多边形" + (dissolve ? "（同值溶解）" : "（逐同值区块）"),
             "值字段 " + field + "；后续可直接接 query.filter / overlay"},
            nullptr};
}

StepResult handle_contour(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "input", "raster.contour"));
    std::vector<float> v = finite_values(rf->data);
    if (v.empty()) throw GisError(E_EMPTY_RESULT, "无有效像元");
    const double lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
    std::vector<double> levels;
    if (has(p, "levels")) {
        for (const auto& x : p["levels"]) levels.push_back(to_double(x, "levels"));
    } else {
        double interval;
        if (has(p, "interval")) {
            interval = to_double(p["interval"], "interval");
        } else {
            if (hi - lo == 0) throw GisError(E_EMPTY_RESULT, "数值无变化，提不出等值线");
            interval = (hi - lo) / 8.0;
        }
        if (!(interval > 0)) throw GisError(E_BAD_REQUEST, "interval 必须为正数");
        for (double x = lo + interval; x < hi; x += interval) levels.push_back(round_to(x, 6));
        if (levels.empty()) throw GisError(E_EMPTY_RESULT, "interval=" + py_float(interval) + " 内没有任何等值线级别");
    }
    const float SENTINEL = -1e30f;
    std::vector<float> data = rf->data;
    for (float& x : data)
        if (!std::isfinite(x)) x = SENTINEL;
    DsPtr src = mem_raster(*rf, data);
    DsPtr vds = mem_vector();
    OGRLayer* lyr = vds->CreateLayer("contour", rf->srs.get(), wkbLineString, nullptr);
    OGRFieldDefn fid("id", OFTInteger), flev("level", OFTReal);
    lyr->CreateField(&fid);
    lyr->CreateField(&flev);
    std::string fixed;
    for (size_t i = 0; i < levels.size(); ++i) fixed += (i ? "," : "") + py_float(levels[i]);
    const std::string o_levels = "FIXED_LEVELS=" + fixed;
    const std::string o_nodata = "NODATA=" + fmt_g(SENTINEL);
    const char* opts[] = {o_levels.c_str(), o_nodata.c_str(), "ID_FIELD=0", "ELEV_FIELD=1", nullptr};
    if (GDALContourGenerateEx(GDALRasterBand::ToHandle(src->GetRasterBand(1)), OGRLayer::ToHandle(lyr), const_cast<char**>(opts), nullptr, nullptr) != CE_None)
        throw GisError(E_BAD_REQUEST, std::string("等值线提取失败（检查 levels）：") + CPLGetLastErrorMsg());

    Layer out;
    out.srs = rf->srs;
    out.fields = {{"level", FieldType::Real}};
    std::set<double> seen;
    lyr->ResetReading();
    for (auto& feat : *lyr) {
        GeomPtr g = clone_geom(feat->GetGeometryRef());
        if (!g || g->IsEmpty()) continue;
        const double lv = feat->GetFieldAsDouble(1);
        seen.insert(lv);
        out.push({lv}, g);
    }
    if (out.empty()) throw GisError(E_EMPTY_RESULT, "没有提取到等值线（levels 可能都在值域外）");
    std::vector<double> shown(seen.begin(), seen.end());
    const bool more = shown.size() > 8;
    if (more) shown.resize(8);
    const size_t n = out.size();
    return {std::make_shared<Layer>(std::move(out)), nullptr,
            {"等值线：" + std::to_string(seen.size()) + " 个级别 / " + std::to_string(n) + " 段线（级别字段 level）",
             "levels=" + py_list(shown) + (more ? "…" : "")},
            nullptr};
}

StepResult handle_sample(Resolver& r, const json& p, const std::string&) {
    auto rf = raster_param(r, need(p, "raster", "raster.sample"));
    auto pts = vector_param(r, need(p, "points", "raster.sample"), "sample.points");
    const std::string field = opt_str(p, "field", "value");
    auto out = std::make_shared<Layer>(*pts);
    const int ci = out->add_field(field, FieldType::Real);
    size_t inside = 0;
    for (size_t i = 0; i < out->size(); ++i) {
        double val = NaN;
        if (const OGRGeometry* g = out->geoms[i].get()) {
            OGRPoint pt;
            g->Centroid(&pt);
            auto [cf, rfl] = rf->to_pixel(pt.getX(), pt.getY());
            const int rr = static_cast<int>(std::floor(rfl)), cc = static_cast<int>(std::floor(cf));
            if (rr >= 0 && rr < rf->height && cc >= 0 && cc < rf->width) {
                const float v = rf->at(rr, cc);
                if (std::isfinite(v)) {
                    val = v;
                    ++inside;
                }
            }
        }
        out->rows[i][ci] = val;
    }
    return {out, nullptr,
            {"栅格采样：" + std::to_string(out->size()) + " 点，" + std::to_string(inside) + " 点命中有效像元（范围外/NoData → NaN）；输出字段 " + field},
            nullptr};
}

}  // namespace

void register_raster_ops(std::map<std::string, OpHandler>& ops) {
    ops["raster.info"] = handle_info;
    ops["raster.create"] = handle_create;
    ops["raster.from_vector"] = handle_from_vector;
    ops["raster.align"] = handle_align;
    ops["raster.mask"] = handle_mask;
    ops["raster.merge"] = handle_merge;
    ops["raster.distance"] = handle_distance;
    ops["raster.distance_decay"] = handle_distance_decay;
    ops["raster.cost_distance"] = handle_cost_distance;
    ops["raster.cost_path"] = handle_cost_path;
    ops["raster.nearest"] = handle_nearest;
    ops["raster.con"] = handle_con;
    ops["raster.calc"] = handle_calc;
    ops["raster.reclassify"] = handle_reclassify;
    ops["raster.breaks"] = handle_breaks;
    ops["raster.histogram"] = handle_histogram;
    ops["raster.weighted_sum"] = handle_weighted_sum;
    ops["raster.focal"] = handle_focal;
    ops["raster.zonal_stats"] = handle_zonal_stats;
    ops["raster.polygonize"] = handle_polygonize;
    ops["raster.contour"] = handle_contour;
    ops["raster.sample"] = handle_sample;
}

}  // namespace gis
