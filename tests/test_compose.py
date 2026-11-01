"""版面合成（制图模式内核侧）测试。"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd
from shapely.geometry import Point, Polygon

from geocore import api
from geocore.runtime.artifact import ArtifactStore


def _districts(p: Path) -> Path:
    gdf = gpd.GeoDataFrame(
        {"name": ["西区", "东区"], "pop": [1200, 3400]},
        geometry=[Polygon([(0, 0), (1, 0), (1, 1), (0, 1)]),
                  Polygon([(1, 0), (2, 0), (2, 1), (1, 1)])],
        crs="EPSG:4326",
    )
    gdf.to_file(p, driver="GeoJSON")
    return p


def _hospitals(p: Path) -> Path:
    gdf = gpd.GeoDataFrame(
        {"name": ["H1", "H2"]},
        geometry=[Point(.3, .5), Point(1.6, .4)],
        crs="EPSG:4326",
    )
    gdf.to_file(p, driver="GeoJSON")
    return p


def _spec(districts: Path, hospitals: Path) -> dict:
    return {
        "title": "临江市医院分布",
        "page": {"size": "A4", "orientation": "landscape"},
        "elements": [
            {"id": "map", "type": "map", "x": .05, "y": .10, "w": .62, "h": .82,
             "extent": [-.05, -.05, 2.05, 1.05],
             "layers": [
                 {"source": str(districts), "label": "行政区", "mode": "choropleth",
                  "field": "pop", "cmap": "YlOrRd"},
                 {"source": str(hospitals), "label": "医院", "mode": "single",
                  "color": "#dc2626"},
             ]},
            {"id": "title", "type": "title", "x": .05, "y": .02, "w": .9, "h": .06,
             "text": "临江市医院分布图"},
            {"id": "legend", "type": "legend", "x": .71, "y": .12, "w": .25, "h": .40},
            {"id": "sb", "type": "scalebar", "x": .08, "y": .93, "w": .22, "h": .04},
            {"id": "na", "type": "north", "x": .90, "y": .85, "w": .05, "h": .09},
            {"id": "note", "type": "text", "x": .71, "y": .90, "w": .25, "h": .06,
             "text": "数据：合成数据\n制图：GeoCore", "size": 7},
        ],
    }


def test_compose_renders_full_layout(tmp_path):
    d = _districts(tmp_path / "districts.geojson")
    h = _hospitals(tmp_path / "hospitals.geojson")
    r = api.compose(tmp_path, {"spec": _spec(d, h), "dpi": 150})
    img = Path(r["image_path"])
    assert img.exists() and img.stat().st_size > 10_000
    assert r["artifact_id"].startswith("ar_")
    # A4 横版 297×210mm @150dpi ≈ 1754×1240
    from PIL import Image

    with Image.open(img) as im:
        assert 1600 < im.width <= 1900
        assert 1150 < im.height <= 1300
    meta = ArtifactStore(tmp_path).get(r["artifact_id"])
    assert meta["kind"] == "map"


def test_compose_categorical_legend_and_missing_layer(tmp_path):
    d = _districts(tmp_path / "districts.geojson")
    h = _hospitals(tmp_path / "hospitals.geojson")
    spec = _spec(d, h)
    spec["elements"][0]["layers"][0] = {
        "source": str(d), "label": "行政区", "mode": "categorical", "field": "name"}
    spec["elements"][0]["layers"].append(
        {"source": str(tmp_path / "不存在.geojson"), "label": "坏层"})
    r = api.compose(tmp_path, {"spec": spec, "dpi": 96})
    assert Path(r["image_path"]).exists()
    assert any("不存在" in w or "加载失败" in w for w in r["warnings"])


def test_compose_raster_layer(tmp_path):
    d = _districts(tmp_path / "districts.geojson")
    heat = api.analyze(tmp_path, {
        "title": "热力",
        "operations": [{"id": "dec", "op": "raster.distance_decay",
                        "input": str(d), "d0": 30_000, "cellsize": 2_000}],
    })
    spec = _spec(d, _hospitals(tmp_path / "h.geojson"))
    spec["elements"][0]["layers"] = [
        {"source": heat["artifact_id"], "label": "可达性", "mode": "continuous",
         "cmap": "YlOrRd"},
        {"source": str(d), "label": "边界", "mode": "single", "color": "#334155",
         "alpha": 0.0},
    ]
    r = api.compose(tmp_path, {"spec": spec, "dpi": 96})
    assert Path(r["image_path"]).exists()
