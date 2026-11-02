"""Generate the deterministic synthetic city '临江市' used by tests/demo/benchmarks.

Everything derives from numpy RNG(seed=42); rerunning reproduces byte-similar
data (timestamps absent). Ground-truth counts are recorded during construction
from grid arithmetic — deliberately NOT derived via the claude_gis stack — so the
benchmark suite can assert against independently obtained numbers.
"""

from __future__ import annotations

import json
from pathlib import Path

import geopandas as gpd
import numpy as np
import pandas as pd
from shapely.geometry import LineString, Point, Polygon, box

SEED = 42
OUT_DIR = Path(__file__).resolve().parents[1] / "datasets" / "synthetic"

# 临江市外框（WGS84）：类似上海附近
LON0, LON1 = 121.35, 121.75
LAT0, LAT1 = 31.12, 31.38

DISTRICT_NAMES = ["城西区", "城东区", "高新区", "滨江区", "城南新区", "老城区"]
LANDUSES = ["居住", "商业", "工业", "绿地"]
ROAD_CLASSES = ["高速", "主干道", "次干道"]

rng = np.random.default_rng(SEED)


def _city_box() -> Polygon:
    return box(LON0, LAT0, LON1, LAT1)


def make_districts() -> gpd.GeoDataFrame:
    nx, ny = 3, 2
    dx, dy = (LON1 - LON0) / nx, (LAT1 - LAT0) / ny
    rows = []
    idx = 0
    for iy in range(ny):
        for ix in range(nx):
            jx, jy = rng.uniform(0.004, 0.011), rng.uniform(0.003, 0.009)
            x0, y0 = LON0 + ix * dx, LAT0 + iy * dy
            x1, y1 = x0 + dx, y0 + dy
            x0j, y0j, x1j, y1j = x0 + jx / 2, y0 + jy / 2, x1 - jx / 2, y1 - jy / 2
            poly = Polygon([
                (x0j, y0j), (x1j, y0j),
                (x1j + rng.uniform(-0.004, 0.004), (y0j + y1j) / 2),
                (x1j, y1j), (x0j, y1j),
                (x0j + rng.uniform(-0.004, 0.004), (y0j + y1j) / 2),
            ]).intersection(_city_box())
            rows.append({
                "district_id": f"D{idx + 1}",
                "name": DISTRICT_NAMES[idx],
                "population": int(rng.integers(25, 130)) * 10000,
                "geometry": poly,
            })
            idx += 1
    return gpd.GeoDataFrame(rows, crs="EPSG:4326")


def make_roads() -> tuple[gpd.GeoDataFrame, dict]:
    cxm, cym = (LON0 + LON1) / 2, (LAT0 + LAT1) / 2
    rows = []

    # 高速：两条贯穿的对角线
    rows.append({"road_id": "R_H1", "class": "高速",
                 "name": "沪临高速", "lanes": 6,
                 "geometry": LineString([(LON0, LAT0), (cxm, cym), (LON1, LAT1)])})
    rows.append({"road_id": "R_H2", "class": "高速",
                 "name": "沿江高速", "lanes": 4,
                 "geometry": LineString([(LON0, LAT1), (cxm, cym), (LON1, LAT0)])})

    # 主干道：两条横、两条纵
    n = 0
    for fy in (0.33, 0.66):
        n += 1
        y = LAT0 + (LAT1 - LAT0) * fy
        rows.append({"road_id": f"R_A{('N','S')[0]}{n}", "class": "主干道",
                     "name": f"临江大道{n}号", "lanes": 6,
                     "geometry": LineString([(LON0, y), (LON1, y)])})
    n = 0
    for fx in (0.33, 0.66):
        n += 1
        x = LON0 + (LON1 - LON0) * fx
        rows.append({"road_id": f"R_AE{n}", "class": "主干道",
                     "name": f"纵一路{n}号", "lanes": 4,
                     "geometry": LineString([(x, LAT0), (x, LAT1)])})

    # 次干道：网格加密
    k = 0
    for fx in np.linspace(0.1, 0.9, 9)[::2]:
        for fy in np.linspace(0.15, 0.85, 6):
            if rng.random() < 0.45:
                continue
            k += 1
            x = LON0 + (LON1 - LON0) * fx
            yy0 = LAT0 + (LAT1 - LAT0) * float(fy)
            rows.append({"road_id": f"R_S{k:02d}", "class": "次干道",
                         "name": f"支路{k:02d}", "lanes": 2,
                         "geometry": LineString([(x - 0.01, yy0), (x + 0.01 + 0.03, yy0 + 0.02)])})
    return gpd.GeoDataFrame(rows, crs="EPSG:4326"), {"secondary_kept": k}


def make_parcels() -> gpd.GeoDataFrame:
    nx, ny = 8, 5
    dx, dy = (LON1 - LON0) / nx, (LAT1 - LAT0) / ny
    weights = [0.42, 0.22, 0.22, 0.14]
    rows = []
    pid = 0
    for iy in range(ny):
        for ix in range(nx):
            pid += 1
            shrink = rng.uniform(0.02, 0.08)
            x0 = LON0 + ix * dx + dx * shrink / 2
            y0 = LAT0 + iy * dy + dy * shrink / 2
            poly = box(x0, y0, x0 + dx * (1 - shrink), y0 + dy * (1 - shrink))
            landuse = str(rng.choice(LANDUSES, p=weights))
            rows.append({
                "parcel_id": f"P{pid:03d}",
                "landuse": landuse,
                "pop_density": int(rng.integers(200, 26000)),
                "geometry": poly,
            })
    return gpd.GeoDataFrame(rows, crs="EPSG:4326")


def _make_river() -> LineString:
    xs = np.linspace(LON0 + 0.02, LON1 - 0.02, 24)
    ys = LAT0 + (LAT1 - LAT0) * (0.28 + 0.16 * np.sin(np.linspace(0, 2.6, 24)))
    return LineString(zip(xs, ys))


def make_hydro(flood_buffer_m: float = 750.0):
    utm_crs = "EPSG:32651"
    river_line = _make_river()
    river = gpd.GeoDataFrame({"river_id": ["长江支流·临江段"], "width_m": [180]},
                             geometry=[river_line], crs="EPSG:4326")
    flood_metric = gpd.GeoSeries([river_line], crs="EPSG:4326").to_crs(utm_crs).buffer(flood_buffer_m)
    flood = gpd.GeoDataFrame(
        {"zone_type": ["洪水风险区"], "return_period_yr": [50]},
        geometry=gpd.GeoSeries(flood_metric, crs=utm_crs).to_crs("EPSG:4326"),
    )
    flood["geometry"] = flood.geometry.intersection(_city_box())
    return river, flood


def make_facilities(districts: gpd.GeoDataFrame) -> tuple[gpd.GeoDataFrame, gpd.GeoDataFrame]:
    def _points(n: int, tag_prefix: str, extra: dict) -> gpd.GeoDataFrame:
        rows = []
        for i in range(n):
            drow = districts.iloc[int(rng.integers(0, len(districts)))]
            minx, miny, maxx, maxy = drow.geometry.bounds
            p = Point(rng.uniform(minx, maxx), rng.uniform(miny, maxy))
            row = {
                "facility_id": f"{tag_prefix}{i + 1:03d}",
                "district": drow["name"],
                **{k: fn(i) for k, fn in extra.items()},
                "geometry": p,
            }
            rows.append(row)
        return gpd.GeoDataFrame(rows, crs="EPSG:4326")

    hospitals = _points(12, "H", {
        "name": lambda i: f"临江市第{i + 1}人民医院" if i % 3 else f"仁济医院临江分院{i // 3 + 1}",
        "beds": lambda i: int(rng.integers(80, 1500)),
        "grade": lambda i: str(rng.choice(["三级甲等", "三级乙等", "二级甲等"], p=[0.3, 0.3, 0.4])),
    })
    schools = _points(25, "S", {
        "name": lambda i: f"临江市实验学校{i + 1}" if i % 2 else f"临江中学{i + 1}",
        "students": lambda i: int(rng.integers(300, 3000)),
        "type": lambda i: str(rng.choice(["小学", "初中", "高中"], p=[0.45, 0.3, 0.25])),
    })
    return hospitals, schools


def make_poi_csv(path: Path, n: int = 60) -> pd.DataFrame:
    cats = ["咖啡", "便利店", "健身房", "书店"]
    df = pd.DataFrame({
        "poi_id": [f"C{i:03d}" for i in range(1, n + 1)],
        "name": [str(rng.choice(cats)) + f"网点{i:03d}" for i in range(1, n + 1)],
        "category": [str(rng.choice(cats, p=[0.3, 0.3, 0.2, 0.2])) for _ in range(n)],
        "lon": rng.uniform(LON0 + 0.02, LON1 - 0.02, n).round(6),
        "lat": rng.uniform(LAT0 + 0.02, LAT1 - 0.02, n).round(6),
        "revenue_k": rng.integers(3, 220, n),
    })
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(path, index=False, encoding="utf-8")
    return df


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    districts = make_districts()
    roads, road_meta = make_roads()
    parcels = make_parcels()
    river, flood = make_hydro()
    hospitals, schools = make_facilities(districts)
    poi_csv_path = OUT_DIR / "poi.csv"
    poi = make_poi_csv(poi_csv_path)

    districts.to_file(OUT_DIR / "districts.geojson", driver="GeoJSON", index=False)
    roads.to_file(OUT_DIR / "roads.geojson", driver="GeoJSON", index=False)
    parcels.to_file(OUT_DIR / "parcels.geojson", driver="GeoJSON", index=False)
    river.to_file(OUT_DIR / "river.geojson", driver="GeoJSON", index=False)
    flood.to_file(OUT_DIR / "flood_zone.geojson", driver="GeoJSON", index=False)
    hospitals.to_file(OUT_DIR / "hospitals.geojson", driver="GeoJSON", index=False)
    schools.to_file(OUT_DIR / "schools.geojson", driver="GeoJSON", index=False)

    truth = {
        "city_bbox_wgs84": [LON0, LAT0, LON1, LAT1],
        "counts_from_construction": {
            "districts": len(DISTRICT_NAMES),
            "roads_all_classes": len(roads),
            "roads_highway": int((roads["class"] == "高速").sum()),
            "roads_arterial": int((roads["class"] == "主干道").sum()),
            "roads_secondary_constructed": road_meta["secondary_kept"],
            "parcels": len(parcels),
            "hospitals": len(hospitals),
            "schools": len(schools),
            "poi": int(len(poi)),
        },
        # 构造期聚合真值：全部要素无条件计入，不依赖空间落位，可与分析结果精确对拍
        "aggregates": {
            "population_total": int(districts["population"].sum()),
            "hospital_beds_total": int(hospitals["beds"].sum()),
            "school_students_total": int(schools["students"].sum()),
            "poi_revenue_total_k": int(poi["revenue_k"].sum()),
            "parcels_by_landuse": {k: int(v) for k, v in parcels["landuse"].value_counts().items()},
        },
        "seed": SEED,
        "crs_note": "全部以 EPSG:4326 存储；洪水区为河流缓冲 750 m（UTM 计算后转回）",
    }
    with open(OUT_DIR / "ground_truth.json", "w", encoding="utf-8") as fh:
        json.dump(truth, fh, ensure_ascii=False, indent=2)

    print(json.dumps(truth["counts_from_construction"], ensure_ascii=False))
    print(f"数据已生成于 {OUT_DIR}")


if __name__ == "__main__":
    main()
