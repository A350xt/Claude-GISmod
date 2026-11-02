#include "crs.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>

#include <cpl_conv.h>

namespace gis {

namespace {
Srs make_srs() {
    auto s = std::make_shared<OGRSpatialReference>();
    s->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return s;
}
}  // namespace

Srs srs_from_user(const std::string& text, const std::string& what) {
    auto s = make_srs();
    if (trim(text).empty() || s->SetFromUserInput(text.c_str()) != OGRERR_NONE) {
        throw GisError(E_BAD_REQUEST,
                       what + " 参数无法解析：'" + text + "'（示例：'EPSG:32622' 或 'source'）",
                       json{{"crs", text}});
    }
    return s;
}

Srs srs_clone(const OGRSpatialReference* srs) {
    if (!srs) return nullptr;
    Srs s(srs->Clone(), [](OGRSpatialReference* p) { p->Release(); });
    s->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return s;
}

Srs srs_epsg(int code) {
    auto s = make_srs();
    s->importFromEPSG(code);
    return s;
}

int srs_epsg_code(const Srs& srs) {
    if (!srs) return 0;
    const char* auth = srs->GetAuthorityName(nullptr);
    const char* code = srs->GetAuthorityCode(nullptr);
    if (auth && code && EQUAL(auth, "EPSG")) return std::atoi(code);
    // 从 Shapefile/.prj 读到的 ESRI WKT 常常不带权威代码：让 PROJ 找最佳匹配
    OGRSpatialReference* best = srs->FindBestMatch(90);
    if (best) {
        int out = 0;
        const char* a2 = best->GetAuthorityName(nullptr);
        const char* c2 = best->GetAuthorityCode(nullptr);
        if (a2 && c2 && EQUAL(a2, "EPSG")) out = std::atoi(c2);
        best->Release();
        return out;
    }
    return 0;
}

std::string srs_string(const Srs& srs) {
    if (!srs) return "";
    if (int code = srs_epsg_code(srs)) return "EPSG:" + std::to_string(code);
    char* proj = nullptr;
    std::string out;
    if (srs->exportToProj4(&proj) == OGRERR_NONE && proj) out = trim(proj);
    CPLFree(proj);
    return out.empty() ? srs_name(srs) : out;
}

std::string srs_name(const Srs& srs) {
    if (!srs) return "";
    const char* n = srs->GetName();
    return n ? n : "";
}

std::string srs_label(const Srs& srs) {
    if (!srs) return "无坐标系";
    std::string name = replace_all(srs_name(srs), "_", " ");
    if (int code = srs_epsg_code(srs)) return "EPSG:" + std::to_string(code) + " · " + name;
    return name.empty() ? srs_string(srs) : name;
}

bool srs_same(const Srs& a, const Srs& b) {
    if (!a || !b) return !a && !b;
    const char* opts[] = {"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", nullptr};
    return a->IsSame(b.get(), opts);
}

bool srs_geographic(const Srs& srs) { return srs && srs->IsGeographic(); }
bool srs_projected(const Srs& srs) { return srs && srs->IsProjected(); }

std::string srs_unit_name(const Srs& srs) {
    if (!srs) return "";
    if (srs->IsGeographic()) return "degree";
    const char* name = nullptr;
    srs->GetLinearUnits(&name);
    return name ? name : "";
}

double srs_unit_meters(const Srs& srs) {
    if (!srs || srs->IsGeographic()) return 0.0;
    return srs->GetLinearUnits(nullptr);
}

Bounds transform_bounds(const Bounds& b, const Srs& from, const Srs& to) {
    if (!from || !to || srs_same(from, to)) return b;
    std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(from.get(), to.get()));
    if (!ct) throw GisError(E_BAD_REQUEST, "无法建立坐标变换：" + srs_string(from) + " → " + srs_string(to));
    double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
    const int n = 21;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double x = b[0] + (b[2] - b[0]) * i / (n - 1);
            double y = b[1] + (b[3] - b[1]) * j / (n - 1);
            if (ct->Transform(1, &x, &y)) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    if (!std::isfinite(minx)) throw GisError(E_BAD_REQUEST, "范围无法变换到目标坐标系");
    return {minx, miny, maxx, maxy};
}

Srs estimate_utm(const Bounds& bounds, const Srs& src) {
    Bounds ll = bounds;
    if (src && !src->IsGeographic()) {
        ll = transform_bounds(bounds, src, srs_epsg(4326));
    } else if (src && srs_epsg_code(src) != 4326) {
        ll = transform_bounds(bounds, src, srs_epsg(4326));
    }
    const double lon = (ll[0] + ll[2]) / 2.0;
    const double lat = (ll[1] + ll[3]) / 2.0;
    if (!std::isfinite(lon) || !std::isfinite(lat) || lat < -84.0 || lat > 84.0) {
        throw GisError(E_CRS_AMBIGUOUS, "无法为该地理范围确定合适的米制投影坐标系",
                       json{{"center", {num(lon), num(lat)}}});
    }
    int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
    zone = std::clamp(zone, 1, 60);
    return srs_epsg((lat >= 0 ? 32600 : 32700) + zone);
}

}  // namespace gis
