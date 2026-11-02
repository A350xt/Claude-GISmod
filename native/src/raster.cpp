#include "raster.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <gdal_alg.h>
#include <gdal_priv.h>
#include <gdalwarper.h>

namespace gis {

namespace {

struct DsCloser {
    void operator()(GDALDataset* ds) const {
        if (ds) GDALClose(GDALDataset::ToHandle(ds));
    }
};
using DsPtr = std::unique_ptr<GDALDataset, DsCloser>;

std::string wkt_of(const Srs& srs) {
    if (!srs) return "";
    char* w = nullptr;
    srs->exportToWkt(&w);
    std::string out = w ? w : "";
    CPLFree(w);
    return out;
}

/// 栅格 → 内存数据集（波段 nodata = NaN）
DsPtr to_mem(const Raster& rf) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    DsPtr ds(mem->Create("", rf.width, rf.height, 1, GDT_Float32, nullptr));
    if (!ds) throw GisError(E_INTERNAL, "无法创建内存栅格");
    GeoTransform gt = rf.gt;
    ds->SetGeoTransform(gt.data());
    if (rf.srs) ds->SetSpatialRef(rf.srs.get());
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->SetNoDataValue(NaN);
    if (band->RasterIO(GF_Write, 0, 0, rf.width, rf.height, const_cast<float*>(rf.data.data()), rf.width, rf.height,
                       GDT_Float32, 0, 0, nullptr) != CE_None)
        throw GisError(E_INTERNAL, "内存栅格写入失败");
    return ds;
}

std::vector<float> read_band(GDALRasterBand* band, int w, int h) {
    std::vector<float> buf(static_cast<size_t>(w) * h);
    if (band->RasterIO(GF_Read, 0, 0, w, h, buf.data(), w, h, GDT_Float32, 0, 0, nullptr) != CE_None)
        throw GisError(E_BAD_REQUEST, std::string("栅格读取失败：") + CPLGetLastErrorMsg());
    return buf;
}

GDALResampleAlg resample_of(const std::string& method) {
    if (method == "nearest") return GRA_NearestNeighbour;
    if (method == "bilinear") return GRA_Bilinear;
    if (method == "cubic") return GRA_Cubic;
    throw GisError(E_BAD_REQUEST, "未知重采样方法：" + method + "（可选 nearest/bilinear/cubic）");
}

void warp_into(const Raster& src, Raster& dst, GDALResampleAlg alg) {
    DsPtr s = to_mem(src);
    DsPtr d = to_mem(dst);  // dst 预填 NaN
    const std::string sw = wkt_of(src.srs), dw = wkt_of(dst.srs);
    CPLErr err = GDALReprojectImage(GDALDataset::ToHandle(s.get()), sw.empty() ? nullptr : sw.c_str(),
                                    GDALDataset::ToHandle(d.get()), dw.empty() ? nullptr : dw.c_str(), alg, 0.0, 0.0,
                                    nullptr, nullptr, nullptr);
    if (err != CE_None) throw GisError(E_BAD_REQUEST, std::string("栅格重投影失败：") + CPLGetLastErrorMsg());
    dst.data = read_band(d->GetRasterBand(1), dst.width, dst.height);
}

}  // namespace

// ---------------------------------------------------------------- Raster

Bounds Raster::bounds() const {
    const double x0 = gt[0], y0 = gt[3];
    const double x1 = gt[0] + width * gt[1] + height * gt[2];
    const double y1 = gt[3] + width * gt[4] + height * gt[5];
    return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

size_t Raster::valid_cells() const {
    return static_cast<size_t>(std::count_if(data.begin(), data.end(), [](float v) { return std::isfinite(v); }));
}

std::pair<double, double> Raster::to_pixel(double x, double y) const {
    // 只支持无旋转网格（本内核产出的栅格均无旋转）
    return {(x - gt[0]) / gt[1], (y - gt[3]) / gt[5]};
}

std::pair<double, double> Raster::center_of(int r, int c) const {
    return {gt[0] + (c + 0.5) * gt[1] + (r + 0.5) * gt[2], gt[3] + (c + 0.5) * gt[4] + (r + 0.5) * gt[5]};
}

std::vector<float> finite_values(const std::vector<float>& data) {
    std::vector<float> v;
    v.reserve(data.size());
    for (float x : data)
        if (std::isfinite(x)) v.push_back(x);
    return v;
}

double quantile_sorted(const std::vector<float>& s, double q) {
    if (s.empty()) return NaN;
    const double pos = q * (s.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, s.size() - 1);
    const double t = pos - lo;
    return s[lo] + (s[hi] - static_cast<double>(s[lo])) * t;
}

json Raster::meta() const {
    std::vector<float> v = finite_values(data);
    std::sort(v.begin(), v.end());
    const double qs[] = {0, .05, .25, .5, .75, .95, 1};
    const char* names[] = {"min", "p05", "p25", "p50", "p75", "p95", "max"};
    json quant = json::object();
    for (int i = 0; i < 7; ++i) quant[names[i]] = num(round_to(quantile_sorted(v, qs[i]), 6));
    const double pct = data.empty() ? 0.0 : 100.0 * (data.size() - v.size()) / data.size();
    return {
        {"shape", {height, width}},
        {"cellsize", num(round_to(cellsize(), 6))},
        {"crs", srs ? json(srs_string(srs)) : json(nullptr)},
        {"nodata_pct", round_to(pct, 2)},
        {"valid_cells", v.size()},
        {"quantiles", quant},
    };
}

std::vector<std::string> Raster::summaries() const {
    const json m = meta();
    const json& q = m["quantiles"];
    auto f = [](const json& v) { return v.is_null() ? std::string("nan") : py_float(v.get<double>()); };
    return {
        "栅格 " + name + "：" + std::to_string(width) + "×" + std::to_string(height) + " 像元，cellsize=" +
            f(m["cellsize"]) + "，CRS=" + (srs ? srs_string(srs) : std::string("None")),
        "有效像元 " + std::to_string(m["valid_cells"].get<size_t>()) + "（NoData " + py_float(m["nodata_pct"].get<double>()) +
            "%）；min=" + f(q["min"]) + " p50=" + f(q["p50"]) + " max=" + f(q["max"]),
    };
}

json Raster::table() const {
    const json m = meta();
    json rows = json::array();
    for (auto it = m["quantiles"].begin(); it != m["quantiles"].end(); ++it) rows.push_back({it.key(), it.value()});
    rows.push_back({"nodata_pct", m["nodata_pct"]});
    rows.push_back({"valid_cells", m["valid_cells"]});
    rows.push_back({"cellsize", m["cellsize"]});
    return {{"columns", {"metric", "value"}}, {"rows", rows}};
}

Raster blank_raster(int height, int width, const GeoTransform& gt, const Srs& srs, float fill, const std::string& name) {
    Raster r;
    r.width = width;
    r.height = height;
    r.gt = gt;
    r.srs = srs;
    r.name = name;
    r.data.assign(static_cast<size_t>(width) * height, fill);
    return r;
}

void check_cells(const Raster& rf) {
    const size_t n = rf.cells();
    if (n == 0) throw GisError(E_BAD_REQUEST, "栅格为空（0 像元）");
    if (n > MAX_CELLS) {
        throw GisError(E_BAD_REQUEST,
                       "栅格过大：" + std::to_string(n) + " 像元超过上限 " + std::to_string(MAX_CELLS) +
                           "；请增大 cellsize 或缩小范围",
                       json{{"cells", n}, {"limit", MAX_CELLS}});
    }
}

// ---------------------------------------------------------------- 读写

bool is_raster_path(const std::string& path) { return ends_with_ci(path, ".tif") || ends_with_ci(path, ".tiff"); }

Raster load_raster(const std::string& path, int band) {
    const fs::path p = u8path(path);
    if (!fs::exists(p)) throw GisError(E_INPUT_MISSING, "栅格不存在：" + native_str(p), json{{"path", native_str(p)}});
    DsPtr ds(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!ds) throw GisError(E_BAD_REQUEST, std::string("栅格读取失败：") + CPLGetLastErrorMsg(), json{{"path", path}});
    const int n = ds->GetRasterCount();
    if (band < 1 || band > n) throw GisError(E_BAD_REQUEST, "波段号 " + std::to_string(band) + " 超出范围（共 " + std::to_string(n) + " 个波段）", json{{"bands", n}});
    Raster r;
    r.width = ds->GetRasterXSize();
    r.height = ds->GetRasterYSize();
    if (ds->GetGeoTransform(r.gt.data()) != CE_None) r.gt = {0, 1, 0, 0, 0, -1};
    r.srs = srs_clone(ds->GetSpatialRef());
    r.name = u8str(p.stem());
    GDALRasterBand* b = ds->GetRasterBand(band);
    r.data = read_band(b, r.width, r.height);
    int has = 0;
    const double nd = b->GetNoDataValue(&has);
    if (has && std::isfinite(nd)) {
        const float ndf = static_cast<float>(nd);
        for (float& v : r.data)
            if (v == ndf) v = static_cast<float>(NaN);
    }
    return r;
}

json raster_meta_of(const std::string& path) {
    Raster r = load_raster(path, 1);
    DsPtr ds(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    GDALRasterBand* b = ds->GetRasterBand(1);
    int has = 0;
    const double nd = b->GetNoDataValue(&has);
    std::vector<float> v = finite_values(r.data);
    std::sort(v.begin(), v.end());
    const auto bd = r.bounds();
    json q = v.empty() ? json(nullptr)
                       : json{{"min", num(quantile_sorted(v, 0))}, {"p50", num(quantile_sorted(v, 0.5))}, {"max", num(quantile_sorted(v, 1))}};
    return {
        {"kind", "raster"},
        {"bands", ds->GetRasterCount()},
        {"shape", {r.height, r.width}},
        {"cellsize", {std::fabs(r.gt[1]), std::fabs(r.gt[5])}},
        {"crs", r.srs ? json(srs_string(r.srs)) : json(nullptr)},
        {"bounds", {bd[0], bd[1], bd[2], bd[3]}},
        {"nodata", has ? num(nd) : json(nullptr)},
        {"dtype", lower(GDALGetDataTypeName(b->GetRasterDataType()))},
        {"nodata_pct", round_to(r.data.empty() ? 0.0 : 100.0 * (r.data.size() - v.size()) / r.data.size(), 2)},
        {"quantiles", q},
    };
}

void save_raster(const Raster& rf, const fs::path& path) {
    check_cells(rf);
    fs::create_directories(path.parent_path());
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    const char* opts[] = {"COMPRESS=DEFLATE", "TILED=YES", nullptr};
    DsPtr ds(drv->Create(u8str(path).c_str(), rf.width, rf.height, 1, GDT_Float32, const_cast<char**>(opts)));
    if (!ds) throw GisError(E_OUTPUT_ERROR, std::string("栅格写出失败：") + CPLGetLastErrorMsg(), json{{"path", native_str(path)}});
    GeoTransform gt = rf.gt;
    ds->SetGeoTransform(gt.data());
    if (rf.srs) ds->SetSpatialRef(rf.srs.get());
    GDALRasterBand* b = ds->GetRasterBand(1);
    b->SetNoDataValue(NaN);
    if (b->RasterIO(GF_Write, 0, 0, rf.width, rf.height, const_cast<float*>(rf.data.data()), rf.width, rf.height,
                    GDT_Float32, 0, 0, nullptr) != CE_None)
        throw GisError(E_OUTPUT_ERROR, std::string("栅格写出失败：") + CPLGetLastErrorMsg());
}

// ---------------------------------------------------------------- 对齐 / 重投影

bool same_grid(const Raster& a, const Raster& b) {
    return a.width == b.width && a.height == b.height && std::fabs(a.gt[0] - b.gt[0]) < 1e-6 &&
           std::fabs(a.gt[3] - b.gt[3]) < 1e-6 && std::fabs(a.gt[1] - b.gt[1]) < 1e-9 && srs_same(a.srs, b.srs);
}

Raster align_to(const Raster& rf, const Raster& tmpl, const std::string& method) {
    const GDALResampleAlg alg = resample_of(method);
    if (same_grid(rf, tmpl)) return rf;
    Raster dst = blank_raster(tmpl.height, tmpl.width, tmpl.gt, tmpl.srs, static_cast<float>(NaN), rf.name);
    warp_into(rf, dst, alg);
    return dst;
}

Raster reproject_raster(const Raster& rf, const Srs& dst_srs) {
    if (!dst_srs || !rf.srs || srs_same(rf.srs, dst_srs)) {
        Raster out = rf;
        if (!out.srs) out.srs = dst_srs;
        return out;
    }
    DsPtr src = to_mem(rf);
    const std::string sw = wkt_of(rf.srs), dw = wkt_of(dst_srs);
    void* tr = GDALCreateGenImgProjTransformer(GDALDataset::ToHandle(src.get()), sw.c_str(), nullptr, dw.c_str(), FALSE, 0.0, 1);
    if (!tr) throw GisError(E_BAD_REQUEST, std::string("栅格重投影失败：") + CPLGetLastErrorMsg());
    GeoTransform gt{};
    int w = 0, h = 0;
    const CPLErr err = GDALSuggestedWarpOutput(GDALDataset::ToHandle(src.get()), GDALGenImgProjTransform, tr, gt.data(), &w, &h);
    GDALDestroyGenImgProjTransformer(tr);
    if (err != CE_None || w <= 0 || h <= 0) throw GisError(E_BAD_REQUEST, "栅格重投影失败：无法确定输出网格");
    Raster dst = blank_raster(h, w, gt, dst_srs, static_cast<float>(NaN), rf.name);
    check_cells(dst);
    warp_into(rf, dst, GRA_NearestNeighbour);
    return dst;
}

std::pair<GeoTransform, std::pair<int, int>> make_transform(const Bounds& b, double cellsize, bool snap) {
    double minx = b[0], miny = b[1], maxx = b[2], maxy = b[3];
    if (!(maxx > minx) || !(maxy > miny))
        throw GisError(E_BAD_REQUEST, "bounds 非法：[" + fmt_g(minx) + ", " + fmt_g(miny) + ", " + fmt_g(maxx) + ", " + fmt_g(maxy) + "]");
    if (!(cellsize > 0)) throw GisError(E_BAD_REQUEST, "cellsize 必须为正数：" + fmt_g(cellsize));
    if (snap) {  // 网格对齐到 cellsize 整数倍，避免不同来源栅格错半像元
        minx = std::floor(minx / cellsize) * cellsize;
        maxy = std::ceil(maxy / cellsize) * cellsize;
    }
    const long long width = std::max(1LL, static_cast<long long>(std::ceil((maxx - minx) / cellsize - 1e-9)));
    const long long height = std::max(1LL, static_cast<long long>(std::ceil((maxy - miny) / cellsize - 1e-9)));
    if (static_cast<double>(width) * height > MAX_CELLS) {
        throw GisError(E_BAD_REQUEST,
                       "栅格模板过大：" + std::to_string(height) + "×" + std::to_string(width) + " 超过 " +
                           std::to_string(MAX_CELLS) + " 像元上限；请增大 cellsize 或缩小范围",
                       json{{"shape", {height, width}}});
    }
    return {{minx, cellsize, 0, maxy, 0, -cellsize}, {static_cast<int>(height), static_cast<int>(width)}};
}

// ---------------------------------------------------------------- 栅格化

std::vector<float> rasterize_values(const std::vector<GeomPtr>& geoms, const std::vector<double>& values,
                                    const GeoTransform& gt, int height, int width, bool all_touched) {
    Raster canvas = blank_raster(height, width, gt, nullptr);
    std::vector<OGRGeometryH> handles;
    std::vector<double> burn;
    for (size_t i = 0; i < geoms.size(); ++i) {
        if (!geoms[i] || geoms[i]->IsEmpty() || !std::isfinite(values[i])) continue;
        handles.push_back(OGRGeometry::ToHandle(geoms[i].get()));
        burn.push_back(values[i]);
    }
    if (handles.empty()) return canvas.data;
    DsPtr ds = to_mem(canvas);
    int band = 1;
    const char* opts[] = {all_touched ? "ALL_TOUCHED=TRUE" : "ALL_TOUCHED=FALSE", nullptr};
    const CPLErr err = GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, &band, static_cast<int>(handles.size()),
                                               handles.data(), nullptr, nullptr, burn.data(), const_cast<char**>(opts),
                                               nullptr, nullptr);
    if (err != CE_None) throw GisError(E_BAD_REQUEST, std::string("栅格化失败：") + CPLGetLastErrorMsg());
    return read_band(ds->GetRasterBand(1), width, height);
}

std::vector<float> rasterize(const Layer& layer, const GeoTransform& gt, int height, int width, const std::string& field,
                             double default_value, bool all_touched) {
    std::vector<double> values(layer.size(), default_value);
    if (!field.empty()) {
        const int fi = layer.field_index(field);
        if (fi < 0) throw GisError(E_INPUT_MISSING, "字段不存在：" + field, json{{"columns", layer.column_names()}});
        for (size_t i = 0; i < layer.size(); ++i) values[i] = as_double(layer.rows[i][fi]);
    }
    return rasterize_values(layer.geoms, values, gt, height, width, all_touched);
}

}  // namespace gis
