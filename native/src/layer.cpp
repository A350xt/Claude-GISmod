#include "layer.hpp"

#include <algorithm>
#include <cstdlib>
#include <charconv>
#include <cstdio>
#include <map>
#include <set>

#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>

namespace gis {

// ================================================================ 值

double as_double(const Value& v) {
    if (auto i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    if (auto d = std::get_if<double>(&v)) return *d;
    if (auto b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
    return NaN;
}

std::string as_text(const Value& v, FieldType type) {
    if (is_null(v)) return "";
    if (type == FieldType::DateTime) return format_datetime(as_double(v));
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto i = std::get_if<std::int64_t>(&v)) return std::to_string(*i);
    if (auto b = std::get_if<bool>(&v)) return *b ? "True" : "False";
    return fmt_g(as_double(v));
}

json to_json(const Value& v, FieldType type) {
    if (is_null(v)) return nullptr;
    if (type == FieldType::DateTime) return format_datetime(as_double(v));
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto b = std::get_if<bool>(&v)) return *b;
    return num(as_double(v));
}

std::string value_key(const Value& v) {
    if (is_null(v)) return "\x01null";
    if (auto s = std::get_if<std::string>(&v)) return "s" + *s;
    if (auto b = std::get_if<bool>(&v)) return *b ? "b1" : "b0";
    double d = as_double(v);
    char buf[64];
    std::snprintf(buf, sizeof buf, "n%.17g", d);
    return buf;
}

namespace {
// Howard Hinnant 的 days_from_civil / civil_from_days
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
void civil_from_days(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}
bool read_int(const std::string& s, size_t& pos, int digits, int& out) {
    if (pos + digits > s.size()) return false;
    out = 0;
    for (int i = 0; i < digits; ++i) {
        char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    pos += digits;
    return true;
}
}  // namespace

std::optional<double> parse_datetime(const std::string& raw) {
    const std::string s = trim(raw);
    size_t p = 0;
    int y, mo, d, h = 0, mi = 0, sec = 0;
    double frac = 0.0;
    if (!read_int(s, p, 4, y) || p >= s.size() || (s[p] != '-' && s[p] != '/')) return std::nullopt;
    const char sep = s[p++];
    if (!read_int(s, p, 2, mo) || p >= s.size() || s[p] != sep) return std::nullopt;
    ++p;
    if (!read_int(s, p, 2, d)) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    if (p < s.size() && (s[p] == 'T' || s[p] == ' ')) {
        ++p;
        if (!read_int(s, p, 2, h) || p >= s.size() || s[p] != ':') return std::nullopt;
        ++p;
        if (!read_int(s, p, 2, mi)) return std::nullopt;
        if (p < s.size() && s[p] == ':') {
            ++p;
            if (!read_int(s, p, 2, sec)) return std::nullopt;
            if (p < s.size() && s[p] == '.') {
                ++p;
                double scale = 0.1;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                    frac += (s[p] - '0') * scale;
                    scale /= 10;
                    ++p;
                }
            }
        }
        if (h > 23 || mi > 59 || sec > 60) return std::nullopt;
    }
    double offset = 0.0;
    if (p < s.size()) {
        if (s[p] == 'Z' || s[p] == 'z') {
            ++p;
        } else if (s[p] == '+' || s[p] == '-') {
            const int sign = s[p] == '-' ? -1 : 1;
            ++p;
            int oh = 0, om = 0;
            if (!read_int(s, p, 2, oh)) return std::nullopt;
            if (p < s.size() && s[p] == ':') ++p;
            if (p < s.size()) read_int(s, p, 2, om);
            // 统一成「无时区的 UTC 时刻」，与 pandas 解析带时区串后再比较的结果一致
            offset = sign * (oh * 3600.0 + om * 60.0);
        }
    }
    if (p != s.size()) return std::nullopt;
    const long long days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return static_cast<double>(days) * 86400.0 + h * 3600.0 + mi * 60.0 + sec + frac - offset;
}

std::string format_datetime(double epoch) {
    if (!std::isfinite(epoch)) return "";
    double whole = std::floor(epoch);
    double frac = epoch - whole;
    long long t = static_cast<long long>(whole);
    long long days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    long long rem = t - days * 86400;
    long long y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld", y, m, d, rem / 3600, (rem % 3600) / 60,
                  rem % 60);
    std::string out = buf;
    if (frac > 1e-6) {
        std::snprintf(buf, sizeof buf, ".%06d", static_cast<int>(std::llround(frac * 1e6)));
        out += buf;
    }
    return out;
}

// ================================================================ 几何

GeomPtr own(OGRGeometry* g) {
    if (!g) return nullptr;
    return GeomPtr(g, [](OGRGeometry* p) { OGRGeometryFactory::destroyGeometry(p); });
}

GeomPtr clone_geom(const OGRGeometry* g) { return g ? own(g->clone()) : nullptr; }

std::string geom_type_name(const OGRGeometry* g) {
    if (!g) return "None";
    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPoint: return "Point";
        case wkbLineString: return "LineString";
        case wkbLinearRing: return "LinearRing";
        case wkbPolygon: return "Polygon";
        case wkbMultiPoint: return "MultiPoint";
        case wkbMultiLineString: return "MultiLineString";
        case wkbMultiPolygon: return "MultiPolygon";
        case wkbGeometryCollection: return "GeometryCollection";
        default: return OGRGeometryTypeToName(g->getGeometryType());
    }
}

Family family_of(const std::string& t) {
    if (t == "Point" || t == "MultiPoint") return Family::Point;
    if (t == "LineString" || t == "MultiLineString" || t == "LinearRing") return Family::Line;
    if (t == "Polygon" || t == "MultiPolygon") return Family::Polygon;
    return Family::Mixed;
}

int geom_dimension(const OGRGeometry* g) { return g ? g->getDimension() : -1; }

double geom_area(const OGRGeometry* g) {
    if (!g) return 0.0;
    return OGR_G_Area(OGRGeometry::ToHandle(const_cast<OGRGeometry*>(g)));
}

double geom_length(const OGRGeometry* g) {
    if (!g) return 0.0;
    const OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
    if (OGR_GT_IsCurve(t)) return g->toCurve()->get_Length();
    if (OGR_GT_IsSurface(t)) {
        double sum = 0.0;
        for (const auto* ring : *g->toCurvePolygon()) sum += ring->get_Length();
        return sum;
    }
    if (OGR_GT_IsSubClassOf(t, wkbGeometryCollection)) {
        double sum = 0.0;
        for (const auto* part : *g->toGeometryCollection()) sum += geom_length(part);
        return sum;
    }
    return 0.0;
}

GeomPtr unary_union(const std::vector<GeomPtr>& geoms) {
    OGRGeometryCollection coll;
    for (const auto& g : geoms)
        if (g && !g->IsEmpty()) coll.addGeometry(g.get());
    if (coll.IsEmpty()) return nullptr;
    return own(coll.UnaryUnion());
}

GeomPtr keep_dimension(GeomPtr g, int dim) {
    if (!g || g->IsEmpty()) return nullptr;
    const OGRwkbGeometryType t = wkbFlatten(g->getGeometryType());
    if (t != wkbGeometryCollection) return g->getDimension() == dim ? g : nullptr;
    std::vector<const OGRGeometry*> parts;
    std::function<void(const OGRGeometry*)> collect = [&](const OGRGeometry* p) {
        if (wkbFlatten(p->getGeometryType()) == wkbGeometryCollection) {
            for (const auto* q : *p->toGeometryCollection()) collect(q);
        } else if (p->getDimension() == dim && !p->IsEmpty()) {
            parts.push_back(p);
        }
    };
    collect(g.get());
    if (parts.empty()) return nullptr;
    if (parts.size() == 1) return clone_geom(parts[0]);
    OGRGeometryCollection* multi = dim == 0   ? static_cast<OGRGeometryCollection*>(new OGRMultiPoint())
                                   : dim == 1 ? static_cast<OGRGeometryCollection*>(new OGRMultiLineString())
                                              : static_cast<OGRGeometryCollection*>(new OGRMultiPolygon());
    for (const auto* p : parts) {
        if (OGR_GT_IsSubClassOf(wkbFlatten(p->getGeometryType()), wkbGeometryCollection)) {
            for (const auto* q : *p->toGeometryCollection()) multi->addGeometry(q);
        } else {
            multi->addGeometry(p);
        }
    }
    return own(multi);
}

// ================================================================ 图层

int Layer::field_index(const std::string& name) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == name) return static_cast<int>(i);
    return -1;
}

int Layer::add_field(const std::string& name, FieldType type) {
    int idx = field_index(name);
    if (idx >= 0) {
        fields[idx].type = type;
        return idx;
    }
    fields.push_back({name, type});
    for (auto& r : rows) r.emplace_back(std::monostate{});
    return static_cast<int>(fields.size()) - 1;
}

std::vector<std::string> Layer::column_names() const {
    std::vector<std::string> out;
    for (const auto& f : fields) out.push_back(f.name);
    out.push_back("geometry");
    return out;
}

Layer Layer::empty_like() const {
    Layer out;
    out.fields = fields;
    out.srs = srs;
    return out;
}

void Layer::push(std::vector<Value> row, GeomPtr geom) {
    row.resize(fields.size());
    rows.push_back(std::move(row));
    geoms.push_back(std::move(geom));
}

Bounds Layer::bounds() const {
    OGREnvelope env;
    bool any = false;
    for (const auto& g : geoms) {
        if (!g || g->IsEmpty()) continue;
        OGREnvelope e;
        g->getEnvelope(&e);
        if (!any) env = e;
        else env.Merge(e);
        any = true;
    }
    if (!any) return {NaN, NaN, NaN, NaN};
    return {env.MinX, env.MinY, env.MaxX, env.MaxY};
}

std::vector<std::string> Layer::geom_types() const {
    std::set<std::string> s;
    for (const auto& g : geoms)
        if (g) s.insert(geom_type_name(g.get()));
    return {s.begin(), s.end()};
}

Family Layer::family() const {
    std::set<Family> fams;
    for (const auto& t : geom_types()) fams.insert(family_of(t));
    if (fams.empty()) return Family::Empty;
    if (fams.size() > 1) return Family::Mixed;
    return *fams.begin();
}

Layer Layer::select(const std::vector<size_t>& idx) const {
    Layer out = empty_like();
    out.rows.reserve(idx.size());
    out.geoms.reserve(idx.size());
    for (size_t i : idx) {
        out.rows.push_back(rows[i]);
        out.geoms.push_back(geoms[i]);
    }
    return out;
}

// ================================================================ 读

namespace {

struct DatasetCloser {
    void operator()(GDALDataset* ds) const { GDALClose(GDALDataset::ToHandle(ds)); }
};
using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

DatasetPtr open_vector(const std::string& path, const char* const* open_options = nullptr, bool update = false) {
    unsigned flags = GDAL_OF_VECTOR | (update ? GDAL_OF_UPDATE : GDAL_OF_READONLY);
    return DatasetPtr(GDALDataset::Open(path.c_str(), flags, nullptr, open_options, nullptr));
}

FieldType map_type(const OGRFieldDefn* defn) {
    switch (defn->GetType()) {
        case OFTInteger:
            return defn->GetSubType() == OFSTBoolean ? FieldType::Boolean : FieldType::Integer;
        case OFTInteger64: return FieldType::Integer;
        case OFTReal: return FieldType::Real;
        case OFTDate:
        case OFTDateTime: return FieldType::DateTime;
        default: return FieldType::String;
    }
}

Value read_field(OGRFeature* f, int i, FieldType type) {
    if (!f->IsFieldSetAndNotNull(i)) return std::monostate{};
    switch (type) {
        case FieldType::Boolean: return f->GetFieldAsInteger(i) != 0;
        case FieldType::Integer: return static_cast<std::int64_t>(f->GetFieldAsInteger64(i));
        case FieldType::Real: return f->GetFieldAsDouble(i);
        case FieldType::DateTime: {
            int y, m, d, h, mi, tz;
            float s;
            if (!f->GetFieldAsDateTime(i, &y, &m, &d, &h, &mi, &s, &tz)) return std::monostate{};
            char buf[48];
            std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%06.3f", y, m, d, h, mi, s);
            auto e = parse_datetime(buf);
            if (!e) return std::monostate{};
            double epoch = *e;
            if (tz > 1) epoch -= (tz - 100) * 15 * 60.0;  // OGR 时区标记：100 = UTC，每单位 15 分钟
            return epoch;
        }
        default: return std::string(f->GetFieldAsString(i));
    }
}

Layer read_ogr_layer(OGRLayer* lyr) {
    Layer out;
    OGRFeatureDefn* defn = lyr->GetLayerDefn();
    for (int i = 0; i < defn->GetFieldCount(); ++i) {
        const OGRFieldDefn* fd = defn->GetFieldDefn(i);
        out.fields.push_back({fd->GetNameRef(), map_type(fd)});
    }
    out.srs = srs_clone(lyr->GetSpatialRef());
    lyr->ResetReading();
    for (auto& feat : *lyr) {
        std::vector<Value> row;
        row.reserve(out.fields.size());
        for (int i = 0; i < static_cast<int>(out.fields.size()); ++i) row.push_back(read_field(feat.get(), i, out.fields[i].type));
        out.rows.push_back(std::move(row));
        out.geoms.push_back(clone_geom(feat->GetGeometryRef()));
    }
    return out;
}

bool parse_int64(const std::string& s, std::int64_t& out) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

const std::vector<std::string> LON_CANDIDATES = {"lon", "lng", "longitude", "x"};
const std::vector<std::string> LAT_CANDIDATES = {"lat", "latitude", "y"};

Layer read_csv_points(const std::string& path) {
    const char* opts[] = {"AUTODETECT_TYPE=NO", "KEEP_GEOM_COLUMNS=YES", "GEOM_POSSIBLE_NAMES=__none__", nullptr};
    DatasetPtr ds = open_vector(path, opts);
    if (!ds || ds->GetLayerCount() == 0) throw GisError(E_BAD_REQUEST, "读取失败:无法打开 CSV " + path, json{{"path", path}});
    OGRLayer* lyr = ds->GetLayer(0);
    OGRFeatureDefn* defn = lyr->GetLayerDefn();
    const int ncol = defn->GetFieldCount();
    std::vector<std::string> names;
    for (int i = 0; i < ncol; ++i) names.push_back(defn->GetFieldDefn(i)->GetNameRef());
    std::vector<std::vector<std::string>> cells;  // [col][row]
    std::vector<std::vector<bool>> present;
    cells.resize(ncol);
    present.resize(ncol);
    lyr->ResetReading();
    for (auto& feat : *lyr) {
        for (int i = 0; i < ncol; ++i) {
            const bool set = feat->IsFieldSetAndNotNull(i);
            std::string v = set ? feat->GetFieldAsString(i) : "";
            present[i].push_back(set && !v.empty());
            cells[i].push_back(std::move(v));
        }
    }
    const size_t nrow = ncol ? cells[0].size() : 0;

    auto find_col = [&](const std::vector<std::string>& cands) -> int {
        for (int i = 0; i < ncol; ++i)
            if (std::find(cands.begin(), cands.end(), lower(trim(names[i]))) != cands.end()) return i;
        return -1;
    };
    const int lon_col = find_col(LON_CANDIDATES), lat_col = find_col(LAT_CANDIDATES);
    if (lon_col < 0 || lat_col < 0) {
        throw GisError(E_BAD_REQUEST, "CSV 文件缺少经纬度列，无法识别为空间数据：" + path,
                       json{{"expected_lon", LON_CANDIDATES}, {"expected_lat", LAT_CANDIDATES}});
    }
    auto in_range = [&](int col, double lim) {
        size_t n = 0;
        for (size_t r = 0; r < nrow; ++r) {
            double v;
            if (!parse_double(trim(cells[col][r]), v)) continue;
            if (v < -lim || v > lim) return false;
            ++n;
        }
        return n > 0;
    };
    if (!in_range(lon_col, 180.0) || !in_range(lat_col, 90.0)) {
        throw GisError(E_CRS_AMBIGUOUS, "CSV 的 " + names[lon_col] + "/" + names[lat_col] + " 列数值不在经纬度范围内，拒绝猜测坐标系",
                       json{{"path", path}});
    }

    Layer out;
    out.srs = srs_epsg(4326);
    std::vector<int> keep;
    for (int i = 0; i < ncol; ++i) {
        if (i == lon_col || i == lat_col) continue;
        // 类型推断：整数 → 实数 → 时间（≥80% 可解析）→ 文本
        bool all_int = true, all_num = true;
        size_t nonnull = 0, as_dt = 0;
        for (size_t r = 0; r < nrow; ++r) {
            if (!present[i][r]) continue;
            ++nonnull;
            const std::string v = trim(cells[i][r]);
            std::int64_t iv;
            double dv;
            if (!parse_int64(v, iv)) all_int = false;
            if (!parse_double(v, dv)) all_num = false;
            if (parse_datetime(v)) ++as_dt;
        }
        FieldType t = FieldType::String;
        if (nonnull > 0 && all_int) t = FieldType::Integer;
        else if (nonnull > 0 && all_num) t = FieldType::Real;
        else if (nonnull > 0 && as_dt * 5 >= nonnull * 4) t = FieldType::DateTime;
        out.fields.push_back({names[i], t});
        keep.push_back(i);
    }
    for (size_t r = 0; r < nrow; ++r) {
        std::vector<Value> row;
        for (size_t k = 0; k < keep.size(); ++k) {
            const int i = keep[k];
            const std::string v = trim(cells[i][r]);
            if (!present[i][r]) {
                row.emplace_back(std::monostate{});
                continue;
            }
            switch (out.fields[k].type) {
                case FieldType::Integer: {
                    std::int64_t iv = 0;
                    parse_int64(v, iv);
                    row.emplace_back(iv);
                    break;
                }
                case FieldType::Real: {
                    double dv = NaN;
                    parse_double(v, dv);
                    row.emplace_back(dv);
                    break;
                }
                case FieldType::DateTime: {
                    auto e = parse_datetime(v);
                    row.emplace_back(e ? Value(*e) : Value(std::monostate{}));
                    break;
                }
                default: row.emplace_back(cells[i][r]);
            }
        }
        double x = NaN, y = NaN;
        parse_double(trim(cells[lon_col][r]), x);
        parse_double(trim(cells[lat_col][r]), y);
        out.rows.push_back(std::move(row));
        out.geoms.push_back(std::isfinite(x) && std::isfinite(y) ? own(new OGRPoint(x, y)) : nullptr);
    }
    return out;
}

}  // namespace

std::vector<std::string> list_layers(const std::string& path) {
    DatasetPtr ds = open_vector(path);
    std::vector<std::string> names;
    if (!ds) return names;
    for (int i = 0; i < ds->GetLayerCount(); ++i) names.push_back(ds->GetLayer(i)->GetName());
    return names;
}

Layer read_vector(const std::string& path, const std::string& layer_name, Provenance& prov) {
    const fs::path p = u8path(path);
    if (!fs::exists(p)) throw GisError(E_INPUT_MISSING, "输入数据不存在：" + native_str(p), json{{"path", native_str(p)}});
    const std::string ext = lower(u8str(p.extension()));
    prov.path = native_str(p);

    if (ext == ".csv") {
        prov.format = "csv+lonlat";
        return read_csv_points(path);
    }
    if (ext != ".geojson" && ext != ".json" && ext != ".gpkg" && ext != ".shp") {
        throw GisError(E_BAD_REQUEST, "不支持的文件格式：" + ext + "（支持 GeoJSON/GeoPackage/Shapefile/CSV+经纬度）",
                       json{{"path", path}});
    }
    DatasetPtr ds = open_vector(path);
    if (!ds) throw GisError(E_BAD_REQUEST, std::string("读取失败:") + CPLGetLastErrorMsg(), json{{"path", path}});

    OGRLayer* lyr = nullptr;
    if (ext == ".gpkg") {
        prov.format = "gpkg";
        std::vector<std::string> names;
        for (int i = 0; i < ds->GetLayerCount(); ++i) names.push_back(ds->GetLayer(i)->GetName());
        if (layer_name.empty()) {
            if (names.empty()) throw GisError(E_BAD_REQUEST, "GeoPackage 中没有图层：" + path, json{{"path", path}});
            lyr = ds->GetLayer(0);
        } else {
            lyr = ds->GetLayerByName(layer_name.c_str());
            if (!lyr) throw GisError(E_INPUT_MISSING, "图层不存在：" + layer_name, json{{"path", path}, {"available_layers", names}});
        }
        prov.layer = lyr->GetName();
    } else {
        if (!layer_name.empty()) throw GisError(E_BAD_REQUEST, ext + " 格式不支持 layer 参数", json{{"path", path}});
        prov.format = (ext == ".shp") ? "shapefile" : "geojson";
        lyr = ds->GetLayer(0);
    }
    if (!lyr) throw GisError(E_BAD_REQUEST, "读取失败:没有图层", json{{"path", path}});
    Layer out = read_ogr_layer(lyr);
    if (out.empty()) throw GisError(E_EMPTY_RESULT, "数据集为空（0 条要素）：" + native_str(p), json{{"path", native_str(p)}});
    if (!out.srs && prov.format == "geojson") out.srs = srs_epsg(4326);  // RFC 7946：GeoJSON 默认 WGS84
    return out;
}

// ================================================================ 写

void write_vector(const Layer& layer, const fs::path& path, const std::string& layer_name, const std::string& driver_name,
                  bool append) {
    const std::string p = u8str(path);
    DatasetPtr ds;
    if (append && fs::exists(path)) {
        ds = open_vector(p, nullptr, true);
    } else {
        GDALDriver* drv = GetGDALDriverManager()->GetDriverByName(driver_name.c_str());
        if (!drv) throw GisError(E_OUTPUT_ERROR, "缺少写出驱动：" + driver_name);
        if (fs::exists(path)) fs::remove(path);
        ds.reset(drv->Create(p.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    }
    if (!ds) throw GisError(E_OUTPUT_ERROR, std::string("结果数据集写出失败:") + CPLGetLastErrorMsg());

    OGRLayer* out = ds->CreateLayer(layer_name.c_str(), layer.srs.get(), wkbUnknown, nullptr);
    if (!out) throw GisError(E_OUTPUT_ERROR, std::string("结果数据集写出失败:") + CPLGetLastErrorMsg());
    for (const auto& f : layer.fields) {
        OGRFieldType t = OFTString;
        OGRFieldSubType st = OFSTNone;
        switch (f.type) {
            case FieldType::Integer: t = OFTInteger64; break;
            case FieldType::Real: t = OFTReal; break;
            case FieldType::DateTime: t = OFTDateTime; break;
            case FieldType::Boolean: t = OFTInteger; st = OFSTBoolean; break;
            default: break;
        }
        OGRFieldDefn defn(f.name.c_str(), t);
        defn.SetSubType(st);
        if (out->CreateField(&defn) != OGRERR_NONE)
            throw GisError(E_OUTPUT_ERROR, "结果数据集写出失败:无法创建字段 " + f.name);
    }
    const bool txn = ds->StartTransaction() == OGRERR_NONE;
    OGRFeatureDefn* defn = out->GetLayerDefn();
    for (size_t r = 0; r < layer.size(); ++r) {
        OGRFeature feat(defn);
        for (size_t i = 0; i < layer.fields.size(); ++i) {
            const Value& v = layer.rows[r][i];
            const int fi = static_cast<int>(i);
            if (is_null(v)) {
                feat.SetFieldNull(fi);
                continue;
            }
            switch (layer.fields[i].type) {
                case FieldType::Integer: feat.SetField(fi, static_cast<GIntBig>(as_double(v))); break;
                case FieldType::Boolean: feat.SetField(fi, as_double(v) != 0 ? 1 : 0); break;
                case FieldType::Real: feat.SetField(fi, as_double(v)); break;
                case FieldType::DateTime: {
                    const std::string iso = format_datetime(as_double(v));
                    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
                    float s = 0;
                    std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%f", &y, &mo, &d, &h, &mi, &s);
                    feat.SetField(fi, y, mo, d, h, mi, s, 0);
                    break;
                }
                default: feat.SetField(fi, as_text(v, FieldType::String).c_str());
            }
        }
        if (layer.geoms[r]) feat.SetGeometry(layer.geoms[r].get());
        if (out->CreateFeature(&feat) != OGRERR_NONE)
            throw GisError(E_OUTPUT_ERROR, std::string("结果数据集写出失败:") + CPLGetLastErrorMsg());
    }
    if (txn) ds->CommitTransaction();
}

// ================================================================ 预览表

json preview_table(const Layer& layer, int limit) {
    const size_t ncol = std::min<size_t>(layer.fields.size(), 12);
    json cols = json::array();
    for (size_t i = 0; i < ncol; ++i) cols.push_back(layer.fields[i].name);
    json rows = json::array();
    const size_t n = std::min<size_t>(layer.size(), static_cast<size_t>(limit));
    for (size_t r = 0; r < n; ++r) {
        json row = json::array();
        for (size_t i = 0; i < ncol; ++i) row.push_back(to_json(layer.rows[r][i], layer.fields[i].type));
        rows.push_back(row);
    }
    json out = {{"columns", cols}, {"rows", rows}, {"row_count_total", layer.size()}};
    out["note"] = layer.size() > static_cast<size_t>(limit)
                      ? json("前 " + std::to_string(n) + " / " + std::to_string(layer.size()) + " 行")
                      : json(nullptr);
    return out;
}

}  // namespace gis
