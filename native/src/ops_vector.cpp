// 矢量原语：Query & Measure / Proximity / Overlay / Zonal / Trajectory。
// 语义与原 Python 内核（geopandas）一致；输入已由 Resolver 统一到分析坐标系并修复几何。
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numeric>
#include <set>

#include "ops.hpp"
#include "where.hpp"

namespace gis {

// ================================================================ 参数工具

const json& need(const json& params, const char* key, const char* op) {
    if (!params.contains(key)) throw GisError(E_BAD_REQUEST, std::string("操作 ") + op + " 缺少必填参数 " + key);
    return params.at(key);
}

bool has(const json& params, const char* key) { return params.contains(key) && !params.at(key).is_null(); }

double to_double(const json& v, const char* key) {
    if (v.is_number()) return v.get<double>();
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string()) {
        const std::string s = trim(v.get<std::string>());
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        if (!s.empty() && end == s.c_str() + s.size()) return d;
    }
    throw GisError(E_BAD_REQUEST, std::string("参数 ") + key + " 需要数字：" + v.dump());
}

int to_int(const json& v, const char* key) { return static_cast<int>(std::trunc(to_double(v, key))); }

bool to_bool(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.is_null();
}

std::string to_str(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

double opt_double(const json& p, const char* key, double fallback) { return has(p, key) ? to_double(p[key], key) : fallback; }
int opt_int(const json& p, const char* key, int fallback) { return has(p, key) ? to_int(p[key], key) : fallback; }
std::string opt_str(const json& p, const char* key, const std::string& fallback) {
    return has(p, key) ? to_str(p[key]) : fallback;
}
std::vector<std::string> opt_str_list(const json& p, const char* key, const std::vector<std::string>& fallback) {
    if (!has(p, key)) return fallback;
    std::vector<std::string> out;
    if (p[key].is_array())
        for (const auto& v : p[key]) out.push_back(to_str(v));
    else
        out.push_back(to_str(p[key]));
    return out;
}

std::string py_list(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", '" : "'") + items[i] + "'";
    return out + "]";
}

std::string py_list(const std::vector<double>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + py_float(items[i]);
    return out + "]";
}

const std::map<std::string, OpHandler>& op_handlers() {
    static const std::map<std::string, OpHandler> ops = [] {
        std::map<std::string, OpHandler> m;
        register_vector_ops(m);
        register_raster_ops(m);
        return m;
    }();
    return ops;
}

namespace {

// ================================================================ 公共

std::shared_ptr<Layer> make_layer(Layer&& l) { return std::make_shared<Layer>(std::move(l)); }

void require_family_uniform(const Layer& layer) {
    if (layer.family() == Family::Mixed) {
        throw GisError(E_BAD_REQUEST, "几何类型混杂（" + py_list(layer.geom_types()) + "），单步运算要求同族几何",
                       json{{"types", layer.geom_types()}});
    }
}

bool env_overlap(const OGRGeometry* a, const OGRGeometry* b) {
    OGREnvelope ea, eb;
    a->getEnvelope(&ea);
    b->getEnvelope(&eb);
    return ea.Intersects(eb);
}

/// 分组键排序：数字按数值、文本按字典序、空值排最后（与 pandas groupby 一致）
bool key_less(const Value& a, const Value& b) {
    const bool na = is_null(a), nb = is_null(b);
    if (na || nb) return !na && nb;
    const bool sa = std::holds_alternative<std::string>(a), sb = std::holds_alternative<std::string>(b);
    if (sa && sb) return std::get<std::string>(a) < std::get<std::string>(b);
    if (sa != sb) return !sa;
    return as_double(a) < as_double(b);
}

/// 两层合并字段：同名列改成 name_a / name_b（geopandas overlay 的 _1/_2 归一化）
struct MergedFields {
    std::vector<FieldDef> fields;
    std::vector<int> map_a, map_b;  // 源字段 → 合并后下标
};

MergedFields merge_fields(const Layer& a, const Layer& b) {
    std::set<std::string> na, nb;
    for (const auto& f : a.fields) na.insert(f.name);
    for (const auto& f : b.fields) nb.insert(f.name);
    MergedFields m;
    for (const auto& f : a.fields) {
        m.map_a.push_back(static_cast<int>(m.fields.size()));
        m.fields.push_back({nb.count(f.name) ? f.name + "_a" : f.name, f.type});
    }
    for (const auto& f : b.fields) {
        m.map_b.push_back(static_cast<int>(m.fields.size()));
        m.fields.push_back({na.count(f.name) ? f.name + "_b" : f.name, f.type});
    }
    return m;
}

std::vector<Value> merged_row(const MergedFields& m, const std::vector<Value>* ra, const std::vector<Value>* rb) {
    std::vector<Value> row(m.fields.size());
    if (ra)
        for (size_t i = 0; i < ra->size(); ++i) row[m.map_a[i]] = (*ra)[i];
    if (rb)
        for (size_t i = 0; i < rb->size(); ++i) row[m.map_b[i]] = (*rb)[i];
    return row;
}

int layer_dimension(const Layer& l) {
    int dim = -1;
    for (const auto& g : l.geoms)
        if (g) dim = std::max(dim, geom_dimension(g.get()));
    return dim;
}

/// 两两求交（keep_dim < 0 时保留所有维度）
template <typename Emit>
void pairwise_intersections(const Layer& a, const Layer& b, int keep_dim, Emit emit) {
    for (size_t i = 0; i < a.size(); ++i) {
        const OGRGeometry* ga = a.geoms[i].get();
        if (!ga) continue;
        for (size_t j = 0; j < b.size(); ++j) {
            const OGRGeometry* gb = b.geoms[j].get();
            if (!gb || !env_overlap(ga, gb) || !ga->Intersects(gb)) continue;
            GeomPtr part = own(ga->Intersection(gb));
            if (keep_dim >= 0) part = keep_dimension(part, keep_dim);
            if (!part || part->IsEmpty()) continue;
            emit(i, j, part);
        }
    }
}

/// a 的每个要素减去与之相交的 b 要素之并
template <typename Emit>
void per_feature_difference(const Layer& a, const Layer& b, int keep_dim, Emit emit) {
    for (size_t i = 0; i < a.size(); ++i) {
        const OGRGeometry* ga = a.geoms[i].get();
        if (!ga) continue;
        std::vector<GeomPtr> hits;
        for (size_t j = 0; j < b.size(); ++j) {
            const OGRGeometry* gb = b.geoms[j].get();
            if (gb && env_overlap(ga, gb) && ga->Intersects(gb)) hits.push_back(b.geoms[j]);
        }
        GeomPtr rest;
        if (hits.empty()) {
            rest = a.geoms[i];
        } else {
            GeomPtr cutter = unary_union(hits);
            rest = cutter ? own(ga->Difference(cutter.get())) : a.geoms[i];
        }
        if (keep_dim >= 0) rest = keep_dimension(rest, keep_dim);
        if (!rest || rest->IsEmpty()) continue;
        emit(i, rest);
    }
}

// ================================================================ Query & Measure

StepResult handle_filter(Resolver& r, const json& p, const std::string&) {
    const std::string where = to_str(need(p, "where", "query.filter"));
    auto gdf = r.frame(to_str(need(p, "input", "query.filter")));
    auto out = make_layer(gdf->select(filter_rows(*gdf, where)));
    return {out, nullptr,
            {"属性过滤 `" + where + "`：" + std::to_string(out->size()) + " 条命中（原 " + std::to_string(gdf->size()) + " 条）"},
            nullptr};
}

const std::vector<std::string> PREDICATES = {"intersects", "within", "contains", "touches", "crosses", "overlaps"};

bool test_predicate(const std::string& pred, const OGRGeometry* g, const OGRGeometry* ref) {
    if (pred == "intersects") return g->Intersects(ref);
    if (pred == "within") return g->Within(ref);
    if (pred == "contains") return g->Contains(ref);
    if (pred == "touches") return g->Touches(ref);
    if (pred == "crosses") return g->Crosses(ref);
    return g->Overlaps(ref);
}

StepResult handle_select(Resolver& r, const json& p, const std::string&) {
    const std::string pred = opt_str(p, "predicate", "intersects");
    if (std::find(PREDICATES.begin(), PREDICATES.end(), pred) == PREDICATES.end())
        throw GisError(E_BAD_REQUEST, "未知空间谓词:" + pred, json{{"allowed", PREDICATES}});
    const std::string ref_tok = to_str(need(p, "ref", "query.select"));
    auto gdf = r.frame(to_str(need(p, "input", "query.select")));
    auto ref = r.frame(ref_tok);
    GeomPtr ref_union = unary_union(ref->geoms);
    std::vector<size_t> keep;
    for (size_t i = 0; i < gdf->size(); ++i)
        if (ref_union && gdf->geoms[i] && test_predicate(pred, gdf->geoms[i].get(), ref_union.get())) keep.push_back(i);
    auto out = make_layer(gdf->select(keep));
    return {out, nullptr,
            {"空间选择（" + pred + " 参照层）命中 " + std::to_string(out->size()) + " 条（原 " + std::to_string(gdf->size()) + " 条）"},
            nullptr};
}

constexpr size_t GROUP_TABLE_LIMIT = 50;

StepResult handle_measure(Resolver& r, const json& p, const std::string&) {
    const auto measures = opt_str_list(p, "measures", {"count"});
    const std::set<std::string> allowed = {"area_sqkm", "area_ha", "length_km", "count"};
    std::vector<std::string> bad;
    for (const auto& m : measures)
        if (!allowed.count(m)) bad.push_back(m);
    if (!bad.empty()) throw GisError(E_BAD_REQUEST, "未知度量:" + py_list(bad), json{{"allowed", allowed}});

    auto gdf = r.frame(to_str(need(p, "input", "query.measure")));
    // 空图层（上一步 allow_empty）没有几何类型可查：度量列照常建出、全为 0
    if (!gdf->empty()) require_family_uniform(*gdf);
    const Family fam = gdf->family();
    auto out = make_layer(Layer(*gdf));
    std::map<std::string, double> totals;
    std::vector<int> value_cols;
    for (const auto& m : measures) {
        if (m == "count") {
            totals["count"] = static_cast<double>(out->size());
            continue;
        }
        const bool is_area = m.rfind("area", 0) == 0;
        if (!gdf->empty() && is_area && fam != Family::Polygon) throw GisError(E_BAD_REQUEST, "面积度量仅适用于面要素");
        if (!gdf->empty() && !is_area && fam != Family::Line) throw GisError(E_BAD_REQUEST, "长度度量仅适用于线要素");
        const double scale = m == "area_sqkm" ? 1e6 : m == "area_ha" ? 1e4 : 1000.0;
        const std::string col = gdf->field_index(m) >= 0 ? m + "_calc" : m;
        const int ci = out->add_field(col, FieldType::Real);
        value_cols.push_back(ci);
        double sum = 0;
        for (size_t i = 0; i < out->size(); ++i) {
            const double v = (is_area ? geom_area(out->geoms[i].get()) : geom_length(out->geoms[i].get())) / scale;
            out->rows[i][ci] = v;
            sum += v;
        }
        totals[m + "_sum"] = sum;
    }
    std::vector<std::string> parts;
    if (totals.count("area_sqkm_sum")) parts.push_back("总面积 " + fmt_fixed(totals["area_sqkm_sum"], 3) + " km²");
    if (totals.count("area_ha_sum")) parts.push_back(fmt_fixed(totals["area_ha_sum"], 1) + " ha");
    if (totals.count("length_km_sum")) parts.push_back("总长度 " + fmt_fixed(totals["length_km_sum"], 3) + " km");
    if (totals.count("count")) parts.push_back("共 " + std::to_string(static_cast<long long>(totals["count"])) + " 条要素");
    std::vector<std::string> summaries = {"度量结果：" + join(parts, "；")};

    json table = nullptr;
    if (has(p, "group_by")) {
        const std::string group_by = to_str(p["group_by"]);
        const int gi = out->field_index(group_by);
        if (gi < 0) throw GisError(E_INPUT_MISSING, "分组字段不存在：" + group_by, json{{"columns", out->column_names()}});
        std::map<std::string, std::vector<size_t>> groups;
        std::map<std::string, Value> keys;
        for (size_t i = 0; i < out->size(); ++i) {
            const std::string k = value_key(out->rows[i][gi]);
            groups[k].push_back(i);
            keys.emplace(k, out->rows[i][gi]);
        }
        std::vector<std::string> order;
        for (const auto& kv : groups) order.push_back(kv.first);
        std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) { return key_less(keys[a], keys[b]); });
        json columns = {group_by, "count"};
        for (int ci : value_cols) columns.push_back(out->fields[ci].name);
        json rows = json::array();
        for (const auto& k : order) {
            const auto& idx = groups[k];
            json row = {to_json(keys[k], out->fields[gi].type), idx.size()};
            for (int ci : value_cols) {
                double s = 0;
                for (size_t i : idx) {
                    const double v = as_double(out->rows[i][ci]);
                    if (std::isfinite(v)) s += v;
                }
                row.push_back(s);
            }
            rows.push_back(row);
        }
        const bool truncated = rows.size() > GROUP_TABLE_LIMIT;
        if (truncated) rows.erase(rows.begin() + GROUP_TABLE_LIMIT, rows.end());
        table = {{"columns", columns}, {"rows", rows}};
        summaries.push_back("按 " + group_by + " 分组明细：" + std::to_string(order.size()) + " 组" +
                            (truncated ? "（表中仅前 " + std::to_string(GROUP_TABLE_LIMIT) + " 组）" : "") +
                            "——每组数值见 steps[].table");
    }
    return {out, nullptr, summaries, table};
}

// ================================================================ Proximity

StepResult handle_buffer(Resolver& r, const json& p, const std::string&) {
    const double distance = to_double(need(p, "distance_m", "proximity.buffer"), "distance_m");
    if (!(distance > 0)) throw GisError(E_BAD_REQUEST, "distance_m 必须为正数（单位：米）");
    const bool dissolve = has(p, "dissolve") && to_bool(p["dissolve"]);
    const int quad_segs = std::max(4, opt_int(p, "quad_segs", 16));
    auto gdf = r.frame(to_str(need(p, "input", "proximity.buffer")));
    std::vector<GeomPtr> buffered;
    for (const auto& g : gdf->geoms) buffered.push_back(g ? own(g->Buffer(distance, quad_segs)) : nullptr);
    if (dissolve) {
        Layer single;
        single.srs = gdf->srs;
        single.fields = {{"distance_m", FieldType::Real}};
        single.push({distance}, unary_union(buffered));
        return {make_layer(std::move(single)), nullptr,
                {"缓冲区 " + fmt_g(distance) + " m：融合为 1 个面（源 " + std::to_string(gdf->size()) + " 个要素）"}, nullptr};
    }
    auto out = make_layer(Layer(*gdf));
    out->geoms = buffered;
    const int ci = out->add_field("buffer_distance_m", FieldType::Real);
    for (auto& row : out->rows) row[ci] = distance;
    return {out, nullptr, {"缓冲区 " + fmt_g(distance) + " m：生成 " + std::to_string(out->size()) + " 个面"}, nullptr};
}

StepResult handle_within_distance(Resolver& r, const json& p, const std::string&) {
    const double distance = to_double(need(p, "distance_m", "proximity.within_distance"), "distance_m");
    auto gdf = r.frame(to_str(need(p, "input", "proximity.within_distance")));
    auto ref = r.frame(to_str(need(p, "ref", "proximity.within_distance")));
    GeomPtr ref_union = unary_union(ref->geoms);
    std::vector<std::pair<double, size_t>> hits;
    for (size_t i = 0; i < gdf->size(); ++i) {
        if (!gdf->geoms[i] || !ref_union) continue;
        const double d = gdf->geoms[i]->Distance(ref_union.get());
        if (d <= distance) hits.emplace_back(d, i);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<size_t> idx;
    for (const auto& h : hits) idx.push_back(h.second);
    auto out = make_layer(gdf->select(idx));
    return {out, nullptr,
            {"邻近筛选：距参照层 " + fmt_g(distance) + " m 以内的要素 " + std::to_string(out->size()) + " 个（原 " +
             std::to_string(gdf->size()) + " 个）"},
            nullptr};
}

StepResult handle_nearest(Resolver& r, const json& p, const std::string&) {
    const int k = opt_int(p, "k", 1);
    const auto join_attrs = opt_str_list(p, "join_attrs", {});
    auto left = r.frame(to_str(need(p, "input", "proximity.nearest")));
    auto right = r.frame(to_str(need(p, "ref", "proximity.nearest")));
    const size_t nl = left->size(), nr = right->size();
    if (nl == 0 || nr == 0)
        throw GisError(E_EMPTY_RESULT, "nearest：输入或参照层为空，无法计算最近邻", json{{"n_left", nl}, {"n_right", nr}});
    if (static_cast<double>(nl) * nr > 25'000'000)
        throw GisError(E_BAD_REQUEST, "最近邻规模过大：" + std::to_string(nl) + "×" + std::to_string(nr) + "；请先缩小输入范围",
                       json{{"n_left", nl}, {"n_right", nr}});
    const size_t kk = static_cast<size_t>(std::clamp(k, 1, static_cast<int>(nr)));

    Layer out = left->empty_like();
    const int rank_col = out.add_field("nearest_rank", FieldType::Integer);
    const int dist_col = out.add_field("nearest_distance_m", FieldType::Real);
    std::vector<std::pair<int, int>> attr_cols;  // right 下标 → out 下标
    for (const auto& a : join_attrs) {
        const int ri = right->field_index(a);
        if (ri >= 0) attr_cols.emplace_back(ri, out.add_field("nearest_" + a, right->fields[ri].type));
    }
    double far = 0;
    std::vector<double> dist(nr);
    std::vector<size_t> order(nr);
    for (size_t i = 0; i < nl; ++i) {
        for (size_t j = 0; j < nr; ++j)
            dist[j] = (left->geoms[i] && right->geoms[j]) ? left->geoms[i]->Distance(right->geoms[j].get()) : INFINITY;
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return dist[a] < dist[b]; });
        for (size_t rank = 0; rank < kk; ++rank) {
            const size_t j = order[rank];
            std::vector<Value> row = left->rows[i];
            row.resize(out.fields.size());
            row[rank_col] = static_cast<std::int64_t>(rank + 1);
            row[dist_col] = dist[j];
            for (const auto& [ri, oi] : attr_cols) row[oi] = right->rows[j][ri];
            far = std::max(far, dist[j]);
            out.push(std::move(row), left->geoms[i]);
        }
    }
    const size_t n = out.size();
    return {make_layer(std::move(out)), nullptr,
            {"最近邻（k=" + std::to_string(k) + "）：输出 " + std::to_string(n) + " 行；最远最近距离 " + fmt_thousands(far) + " m"},
            nullptr};
}

// ================================================================ Overlay

std::pair<std::shared_ptr<Layer>, std::shared_ptr<Layer>> both(Resolver& r, const json& p, const char* op) {
    auto a = r.frame(to_str(need(p, "a", op)));
    auto b = r.frame(to_str(need(p, "b", op)));
    return {a, b};
}

StepResult handle_intersection(Resolver& r, const json& p, const std::string&) {
    const auto ab = both(r, p, "overlay.intersection");
    const auto& a = ab.first;
    const auto& b = ab.second;
    const MergedFields m = merge_fields(*a, *b);
    Layer out;
    out.fields = m.fields;
    out.srs = a->srs;
    pairwise_intersections(*a, *b, layer_dimension(*a), [&](size_t i, size_t j, GeomPtr g) {
        out.push(merged_row(m, &a->rows[i], &b->rows[j]), g);
    });
    const size_t n = out.size();
    return {make_layer(std::move(out)), nullptr, {"叠加求交：结果 " + std::to_string(n) + " 个要素"}, nullptr};
}

StepResult handle_difference(Resolver& r, const json& p, const std::string&) {
    const auto ab = both(r, p, "overlay.difference");
    const auto& a = ab.first;
    const auto& b = ab.second;
    Layer out = a->empty_like();
    per_feature_difference(*a, *b, layer_dimension(*a), [&](size_t i, GeomPtr g) { out.push(a->rows[i], g); });
    const size_t n = out.size();
    return {make_layer(std::move(out)), nullptr,
            {"叠加扣除（A−B）：从 " + std::to_string(a->size()) + " 个 A 要素得到 " + std::to_string(n) + " 个剩余部分"}, nullptr};
}

StepResult handle_union(Resolver& r, const json& p, const std::string&) {
    const auto ab = both(r, p, "overlay.union");
    const auto& a = ab.first;
    const auto& b = ab.second;
    const MergedFields m = merge_fields(*a, *b);
    Layer out;
    out.fields = m.fields;
    out.srs = a->srs;
    pairwise_intersections(*a, *b, -1, [&](size_t i, size_t j, GeomPtr g) {
        out.push(merged_row(m, &a->rows[i], &b->rows[j]), g);
    });
    per_feature_difference(*a, *b, -1, [&](size_t i, GeomPtr g) { out.push(merged_row(m, &a->rows[i], nullptr), g); });
    per_feature_difference(*b, *a, -1, [&](size_t j, GeomPtr g) { out.push(merged_row(m, nullptr, &b->rows[j]), g); });
    const size_t n = out.size();
    return {make_layer(std::move(out)), nullptr, {"叠加合并（A∪B）：结果 " + std::to_string(n) + " 个要素"}, nullptr};
}

StepResult handle_clip(Resolver& r, const json& p, const std::string&) {
    auto a = r.frame(to_str(need(p, "a", "overlay.clip")));
    auto mask = r.frame(to_str(need(p, "b", "overlay.clip")));
    Layer out = a->empty_like();
    pairwise_intersections(*a, *mask, layer_dimension(*a), [&](size_t i, size_t, GeomPtr g) { out.push(a->rows[i], g); });
    const size_t n = out.size();
    return {make_layer(std::move(out)), nullptr,
            {"按掩膜裁剪：保留 " + std::to_string(n) + " 个要素（原 " + std::to_string(a->size()) + " 个）"}, nullptr};
}

// ================================================================ Zonal

StepResult handle_zonal(Resolver& r, const json& p, const std::string&) {
    auto regions = r.frame(to_str(need(p, "regions", "zonal.summarize")));
    auto data = r.frame(to_str(need(p, "data", "zonal.summarize")));
    const auto stats_list = opt_str_list(p, "stats", {"count"});
    const std::set<std::string> wanted(stats_list.begin(), stats_list.end());
    const std::string field = has(p, "field") ? to_str(p["field"]) : "";

    if (regions->empty() || data->empty()) {
        const int pos = regions->empty() ? 0 : 1;
        throw GisError(E_EMPTY_RESULT, "zonal.summarize: 输入为空，无法继续运算（第 " + std::to_string(pos + 1) + " 个输入）",
                       json{{"position", pos}});
    }
    require_family_uniform(*data);
    const std::set<std::string> known = {"count", "sum", "mean", "min", "max", "total_length_km", "total_area_sqkm", "share_pct"};
    std::vector<std::string> unknown;
    for (const auto& s : wanted)
        if (!known.count(s)) unknown.push_back(s);
    if (!unknown.empty()) throw GisError(E_BAD_REQUEST, "未知统计量:" + py_list(unknown), json{{"known", known}});
    if ((wanted.count("sum") || wanted.count("mean")) && field.empty())
        throw GisError(E_BAD_REQUEST, "sum/mean 统计需要提供 field 字段名");
    const int fi = field.empty() ? -1 : data->field_index(field);
    if (!field.empty() && fi < 0) throw GisError(E_INPUT_MISSING, "字段不存在:" + field, json{{"columns", data->column_names()}});

    const Family fam = data->family();
    const int dim = fam == Family::Point ? 0 : fam == Family::Line ? 1 : 2;

    struct Part {
        size_t didx, ridx;
        GeomPtr geom;
    };
    std::vector<Part> parts;
    pairwise_intersections(*data, *regions, dim, [&](size_t i, size_t j, GeomPtr g) { parts.push_back({i, j, g}); });

    auto out = make_layer(Layer(*regions));
    const size_t nreg = out->size();
    std::vector<std::vector<const Part*>> by_region(nreg);
    for (const auto& pt : parts) by_region[pt.ridx].push_back(&pt);
    auto field_val = [&](size_t didx) { return fi >= 0 ? as_double(data->rows[didx][fi]) : NaN; };

    const int count_col = out->add_field("data_count", FieldType::Integer);
    json info = {{"kind", fam == Family::Point ? "point" : fam == Family::Line ? "line" : "polygon"}};

    if (fam == Family::Point) {
        for (size_t j = 0; j < nreg; ++j) out->rows[j][count_col] = static_cast<std::int64_t>(by_region[j].size());
        if (fi >= 0) {
            const bool want_sum = wanted.count("sum"), want_mean = wanted.count("mean"), want_min = wanted.count("min"),
                       want_max = wanted.count("max");
            const int c_sum = want_sum ? out->add_field("data_" + field + "_sum", FieldType::Real) : -1;
            const int c_mean = want_mean ? out->add_field("data_" + field + "_mean", FieldType::Real) : -1;
            const int c_min = want_min ? out->add_field("data_" + field + "_min", FieldType::Real) : -1;
            const int c_max = want_max ? out->add_field("data_" + field + "_max", FieldType::Real) : -1;
            for (size_t j = 0; j < nreg; ++j) {
                double s = 0, mn = INFINITY, mx = -INFINITY;
                size_t n = 0;
                for (const Part* pt : by_region[j]) {
                    const double v = field_val(pt->didx);
                    if (!std::isfinite(v)) continue;
                    s += v;
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                    ++n;
                }
                if (c_sum >= 0) out->rows[j][c_sum] = s;
                if (c_mean >= 0) out->rows[j][c_mean] = n ? s / n : NaN;
                if (c_min >= 0) out->rows[j][c_min] = n ? mn : NaN;
                if (c_max >= 0) out->rows[j][c_max] = n ? mx : NaN;
            }
        }
        info["grand_total"] = data->size();
        info["matched"] = parts.size();
    } else {
        const bool is_line = fam == Family::Line;
        auto measure = [&](const OGRGeometry* g) { return is_line ? geom_length(g) : geom_area(g); };
        std::vector<double> full(data->size());
        double total = 0;
        for (size_t i = 0; i < data->size(); ++i) {
            full[i] = measure(data->geoms[i].get());
            total += full[i];
        }
        const bool want_measure = wanted.count(is_line ? "total_length_km" : "total_area_sqkm") || wanted.count("share_pct");
        const std::string base = is_line ? "data_length_km" : "data_area_sqkm";
        const double unit = is_line ? 1000.0 : 1e6;
        const int c_measure = want_measure ? out->add_field(base, FieldType::Real) : -1;
        const int c_share = want_measure && wanted.count("share_pct")
                                ? out->add_field(is_line ? "data_length_share_pct" : "data_area_share_pct", FieldType::Real)
                                : -1;
        const int c_sum = (fi >= 0 && wanted.count("sum")) ? out->add_field("data_" + field + "_sum_apportioned", FieldType::Real) : -1;
        const int c_wmean = (!is_line && fi >= 0 && wanted.count("mean"))
                                ? out->add_field("data_" + field + "_area_weighted_mean", FieldType::Real)
                                : -1;
        for (size_t j = 0; j < nreg; ++j) {
            std::set<size_t> distinct;
            double hit = 0, contrib = 0, wsum = 0, wnum = 0;
            for (const Part* pt : by_region[j]) {
                distinct.insert(pt->didx);
                const double m = measure(pt->geom.get());
                hit += m;
                const double frac = m / (full[pt->didx] == 0 ? 1.0 : full[pt->didx]);
                const double v = field_val(pt->didx);
                if (std::isfinite(v)) {
                    contrib += v * frac;
                    wnum += v * frac;
                }
                wsum += frac;
            }
            out->rows[j][count_col] = static_cast<std::int64_t>(distinct.size());
            if (c_measure >= 0) out->rows[j][c_measure] = hit / unit;
            if (c_share >= 0) out->rows[j][c_share] = hit / std::max(total, 1e-9) * 100.0;
            if (c_sum >= 0) out->rows[j][c_sum] = contrib;
            if (c_wmean >= 0) out->rows[j][c_wmean] = by_region[j].empty() ? NaN : wnum / std::max(wsum, 1e-12);
        }
        info[is_line ? "grand_total_length_km" : "grand_total_area_sqkm"] = round_to(total / unit, 6);
    }

    size_t covered = 0;
    for (size_t j = 0; j < nreg; ++j)
        if (as_double(out->rows[j][count_col]) > 0) ++covered;
    static const std::map<std::string, std::string> LABEL = {{"point", "点要素"}, {"line", "线要素"}, {"polygon", "面要素"}};
    std::vector<std::string> lines = {"分区统计（" + LABEL.at(info["kind"].get<std::string>()) + " × " + std::to_string(nreg) +
                                      " 个区域）：覆盖区域 " + std::to_string(covered) + " 个，未覆盖 " +
                                      std::to_string(nreg - covered) + " 个"};
    if (info.contains("grand_total"))
        lines.push_back("数据层共 " + info["grand_total"].dump() + " 条，其中落进任意区域的 " + info["matched"].dump() + " 条");
    auto column_sum = [&](const std::string& name) {
        const int c = out->field_index(name);
        double s = 0;
        if (c >= 0)
            for (const auto& row : out->rows) s += std::isfinite(as_double(row[c])) ? as_double(row[c]) : 0;
        return s;
    };
    if (info.contains("grand_total_length_km"))
        lines.push_back("落入区域的线总长 " + fmt_fixed(column_sum("data_length_km"), 3) + " km（全层 " +
                        py_float(info["grand_total_length_km"].get<double>()) + " km）");
    if (info.contains("grand_total_area_sqkm"))
        lines.push_back("落入区域的面合计 " + fmt_fixed(column_sum("data_area_sqkm"), 3) + " km²（全层 " +
                        py_float(info["grand_total_area_sqkm"].get<double>()) + " km²）");
    return {out, nullptr, lines, nullptr};
}

// ================================================================ Trajectory

constexpr size_t TRACK_SUMMARY_LIMIT = 20;

StepResult handle_trajectory(Resolver& r, const json& p, const std::string&) {
    const std::string time_field = to_str(need(p, "time_field", "trajectory.build"));
    auto gdf = r.frame(to_str(need(p, "input", "trajectory.build")));
    const int ti = gdf->field_index(time_field);
    if (ti < 0) throw GisError(E_INPUT_MISSING, "时间字段不存在：" + time_field, json{{"columns", gdf->column_names()}});

    std::vector<double> ts(gdf->size(), NaN);
    for (size_t i = 0; i < gdf->size(); ++i) {
        const Value& v = gdf->rows[i][ti];
        if (gdf->fields[ti].type == FieldType::DateTime) ts[i] = as_double(v);
        else if (auto e = parse_datetime(as_text(v, gdf->fields[ti].type))) ts[i] = *e;
    }
    const size_t n_ok = static_cast<size_t>(std::count_if(ts.begin(), ts.end(), [](double t) { return std::isfinite(t); }));
    if (n_ok < 2) {
        throw GisError(E_BAD_REQUEST,
                       "time_field=" + time_field + " 无法解析为时间（有效 " + std::to_string(n_ok) + " / " +
                           std::to_string(gdf->size()) + " 条），请检查格式（支持 ISO 8601，如 2024-09-01T12:00:00）",
                       json{{"time_field", time_field}});
    }
    const bool grouped = has(p, "group_by");
    const std::string group_by = grouped ? to_str(p["group_by"]) : "";
    const int gi = grouped ? gdf->field_index(group_by) : -1;
    if (grouped && gi < 0) throw GisError(E_INPUT_MISSING, "分组字段不存在：" + group_by, json{{"columns", gdf->column_names()}});

    std::vector<size_t> order;
    for (size_t i = 0; i < gdf->size(); ++i)
        if (std::isfinite(ts[i])) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ts[a] < ts[b]; });

    // 分组：有 group_by 按字段值，否则按日期
    std::map<std::string, std::vector<size_t>> groups;
    std::map<std::string, Value> keys;
    for (size_t i : order) {
        Value key = grouped ? gdf->rows[i][gi] : Value(format_datetime(std::floor(ts[i] / 86400.0) * 86400.0).substr(0, 10));
        const std::string k = value_key(key);
        groups[k].push_back(i);
        keys.emplace(k, key);
    }
    std::vector<std::string> gorder;
    for (const auto& kv : groups) gorder.push_back(kv.first);
    std::sort(gorder.begin(), gorder.end(), [&](const auto& a, const auto& b) { return key_less(keys[a], keys[b]); });

    const std::string label_col = grouped ? group_by : "track";
    const FieldType label_type = grouped ? gdf->fields[gi].type : FieldType::String;
    Layer out;
    out.srs = gdf->srs;
    out.fields = {{label_col, label_type},
                  {"point_count", FieldType::Integer},
                  {"start_time", FieldType::DateTime},
                  {"end_time", FieldType::DateTime},
                  {"duration_h", FieldType::Real}};
    size_t skipped = 0;
    json table_rows = json::array();
    for (const auto& k : gorder) {
        const auto& idx = groups[k];
        if (idx.size() < 2) {
            ++skipped;
            continue;
        }
        auto* line = new OGRLineString();
        for (size_t i : idx) {
            const OGRGeometry* g = gdf->geoms[i].get();
            if (g && wkbFlatten(g->getGeometryType()) == wkbPoint) line->addPoint(g->toPoint()->getX(), g->toPoint()->getY());
        }
        const double start = ts[idx.front()], end = ts[idx.back()];
        const double hours = round_to((end - start) / 3600.0, 4);
        out.push({keys[k], static_cast<std::int64_t>(idx.size()), start, end, hours}, own(line));
        if (table_rows.size() < TRACK_SUMMARY_LIMIT) {
            table_rows.push_back({to_json(keys[k], label_type), idx.size(), format_datetime(start), format_datetime(end), hours});
        }
    }
    if (out.empty()) {
        throw GisError(E_EMPTY_RESULT, "没有任何分组能构成轨迹（每组至少需要 2 个带有效时间的点）", json{{"skipped_groups", skipped}});
    }
    std::vector<std::string> summaries = {"轨迹构建：" + std::to_string(out.size()) + " 条（按 " +
                                          (grouped ? group_by : std::string("全部点连线")) + "，时间字段 " + time_field +
                                          "，有效点 " + std::to_string(n_ok) + "/" + std::to_string(gdf->size()) + "）"};
    if (skipped) summaries.push_back(std::to_string(skipped) + " 个分组点数不足 2，已跳过");
    json table = {{"columns", {label_col, "point_count", "start_time", "end_time", "duration_h"}}, {"rows", table_rows}};
    if (out.size() > TRACK_SUMMARY_LIMIT)
        table["note"] = "表中仅前 " + std::to_string(TRACK_SUMMARY_LIMIT) + " / " + std::to_string(out.size()) + " 条";
    return {make_layer(std::move(out)), nullptr, summaries, table};
}

}  // namespace

void register_vector_ops(std::map<std::string, OpHandler>& ops) {
    ops["query.filter"] = handle_filter;
    ops["query.select"] = handle_select;
    ops["query.measure"] = handle_measure;
    ops["proximity.buffer"] = handle_buffer;
    ops["proximity.within_distance"] = handle_within_distance;
    ops["proximity.nearest"] = handle_nearest;
    ops["overlay.intersection"] = handle_intersection;
    ops["overlay.difference"] = handle_difference;
    ops["overlay.union"] = handle_union;
    ops["overlay.clip"] = handle_clip;
    ops["zonal.summarize"] = handle_zonal;
    ops["trajectory.build"] = handle_trajectory;
}

}  // namespace gis
