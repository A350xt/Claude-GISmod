"""栅格域测试：22 个 raster.* 算子的主链与语义对拍（对标 Phase 2 Field GIS 核心）。"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd
import numpy as np
import pytest
from shapely.geometry import Point, Polygon

from claude_gis import api
from claude_gis.analysis.executor import execute_analyze
from claude_gis.protocol import GisError
from claude_gis.raster_core import load_raster


# ------------------------------------------------------------------ fixtures

@pytest.fixture(scope="module")
def two_points(tmp_path_factory) -> Path:
    """两个相距 1000m 的点（EPSG:32650 投影坐标，免重投影干扰对拍）。"""
    d = tmp_path_factory.mktemp("ras")
    gdf = gpd.GeoDataFrame(
        {"name": ["A", "B"]},
        geometry=[Point(500_000, 3_400_000), Point(501_000, 3_400_000)],
        crs="EPSG:32650",
    )
    p = d / "pts.geojson"
    gdf.to_file(p, driver="GeoJSON")
    return p


@pytest.fixture(scope="module")
def square(tmp_path_factory) -> Path:
    d = tmp_path_factory.mktemp("ras")
    gdf = gpd.GeoDataFrame(
        {"v": [10.0, 20.0]},
        geometry=[Polygon([(0, 0), (5, 0), (5, 5), (0, 5)]),
                  Polygon([(5, 5), (10, 5), (10, 10), (5, 10)])],
        crs="EPSG:32650",
    )
    p = d / "sq.geojson"
    gdf.to_file(p, driver="GeoJSON")
    return p


def _op(workdir, ops, title="raster-test", **extra):
    return execute_analyze(workdir, {"title": title, "operations": ops, **extra})


# ------------------------------------------------------------------- 基建

def test_from_vector_and_info(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector",
         "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "i", "op": "raster.info", "input": "@g"},
    ])
    assert r["result"]["format"] == "tif"
    assert r["result"]["geometry_types"] == ["Raster"]
    table = r["steps"][1]["table"]
    assert dict(zip(table["columns"], table["rows"][0]))["metric"] == "min"
    # 结果 tif 落盘且可回读
    rf = load_raster(r["result"]["path"])
    assert rf.data.shape == (10, 10)
    assert float(np.nanmin(rf.data)) == 10.0


def test_artifact_step_tif_roundtrip(square, tmp_path):
    """跨请求引用：上一次分析的栅格步骤 ar_xxx#g 在新请求里可回读。"""
    r1 = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0},
    ])
    art = r1["artifact_id"]
    r2 = _op(tmp_path, [
        {"id": "c", "op": "raster.con", "input": f"{art}#g",
         "condition": "value >= 1", "true": 7, "false": 0},
    ])
    rf = load_raster(r2["result"]["path"])
    assert set(np.unique(rf.data[np.isfinite(rf.data)])) == {7.0}


def test_create_and_merge(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "a", "op": "raster.create", "bounds": [0, 0, 10, 10],
         "cellsize": 1, "crs": "EPSG:32650", "value": 5},
        {"id": "b", "op": "raster.create", "bounds": [8, 0, 18, 10],
         "cellsize": 1, "crs": "EPSG:32650", "value": 9},
        {"id": "m", "op": "raster.merge", "inputs": ["@a", "@b"]},
    ])
    rf = load_raster(r["result"]["path"])
    vals = rf.data[np.isfinite(rf.data)]
    assert set(np.unique(vals)) == {5.0, 9.0}   # 重叠区先到先得
    assert rf.data.shape == (10, 18)


def test_mask_vector_fill(square, tmp_path):
    hole = tmp_path / "hole.geojson"
    gpd.GeoDataFrame(geometry=[Polygon([(0, 0), (4, 0), (4, 4), (0, 4)])],
                     crs="EPSG:32650").to_file(hole, driver="GeoJSON")
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0},
        {"id": "m", "op": "raster.mask", "input": "@g", "mask": str(hole),
         "fill": 0},
    ])
    rf = load_raster(r["result"]["path"])
    assert float(np.nanmin(rf.data)) == 0.0  # 掩膜外填 0，无 NoData
    assert float(np.nanmax(rf.data)) == 1.0


def test_align_warns_on_mismatch(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0},
        {"id": "c", "op": "raster.create", "bounds": [0, 0, 10, 10],
         "cellsize": 2, "crs": "EPSG:32650", "value": 3},
        {"id": "w", "op": "raster.weighted_sum",
         "inputs": [{"ref": "@g"}, {"ref": "@c", "weight": 1}]},
    ])
    assert any("自动重采样对齐" in w for w in r["warnings"])


# ------------------------------------------------------------------- 距离

def test_distance_field_semantics(two_points, tmp_path):
    """两点相距 1000m：源处=0，中位距离在两点间距的一半量级。"""
    r = _op(tmp_path, [
        {"id": "d", "op": "raster.distance",
         "input": str(two_points), "cellsize": 50},
    ])
    rf = load_raster(r["result"]["path"])
    assert float(np.nanmin(rf.data)) == 0.0
    assert 100.0 < float(np.nanmedian(rf.data)) < 500.0


def test_distance_decay_modes(two_points, tmp_path):
    common = [{"id": "s", "op": "raster.distance_decay",
               "input": str(two_points), "d0": 500, "f0": 100, "cellsize": 50}]
    lin = _op(tmp_path, common, title="linear")["result"]["path"]
    sq = _op(tmp_path, common + [], title="square", mode="square") if False else None
    lin_rf = load_raster(lin)
    v = lin_rf.data[np.isfinite(lin_rf.data)]
    assert float(np.nanmax(v)) == 100.0
    assert float(np.nanmin(v)) == 0.0  # >d0 处归 0
    # square 模式单独跑（params 在 op 层）
    r2 = _op(tmp_path, [
        {"id": "s", "op": "raster.distance_decay", "input": str(two_points),
         "d0": 500, "f0": 100, "cellsize": 50, "mode": "square"},
    ], title="square")
    sq_rf = load_raster(r2["result"]["path"])
    sv = sq_rf.data[np.isfinite(sq_rf.data)]
    assert float(np.nanmax(sv)) == 100.0
    # 同距离处 square ≥ linear（衰减更慢）
    mid = (lin_rf.data + sq_rf.data) / 2
    finite = np.isfinite(mid)
    assert (sq_rf.data[finite] >= lin_rf.data[finite] - 1e-6).all()


def test_cost_distance_and_path(two_points, tmp_path):
    """匀速成本面=1：成本距离≈欧氏距离；路径连通。"""
    r_cost = _op(tmp_path, [
        {"id": "plane", "op": "raster.create", "bounds": [499_000, 3_399_500, 501_500, 3_400_500],
         "cellsize": 50, "crs": "EPSG:32650", "value": 1},
        {"id": "cd", "op": "raster.cost_distance",
         "source": str(two_points), "cost": "@plane"},
    ])
    rf = load_raster(r_cost["result"]["path"])
    assert float(np.nanmin(rf.data)) == 0.0
    # 最远角直线距离 ≈1084m，8 邻接 Dijkstra 有阶梯开销 → 1000~1400 区间
    assert 1000.0 <= float(np.nanmax(rf.data)) <= 1400.0

    target = tmp_path / "to.geojson"
    gpd.GeoDataFrame(geometry=[Point(501_000, 3_400_500)], crs="EPSG:32650").to_file(
        target, driver="GeoJSON")
    r_path = _op(tmp_path, [
        {"id": "plane", "op": "raster.create", "bounds": [499_000, 3_399_500, 501_500, 3_400_500],
         "cellsize": 50, "crs": "EPSG:32650", "value": 1},
        {"id": "cp", "op": "raster.cost_path",
         "source": str(two_points), "cost": "@plane", "to": str(target)},
    ])
    assert r_path["result"]["geometry_types"] == ["LineString"]
    # 匀速面：总成本 ≈ 欧氏长度（500 + 500 直角）
    assert r_path["steps"][1]["summary"].startswith("成本最短路径")


def test_nearest_allocation(two_points, tmp_path):
    r = _op(tmp_path, [
        {"id": "n", "op": "raster.nearest",
         "input": str(two_points), "ref": str(two_points), "cellsize": 100},
    ])
    rf = load_raster(r["result"]["path"])
    ids = np.unique(rf.data[np.isfinite(rf.data)])
    assert set(ids) <= {1.0, 2.0}
    assert 1.0 in ids and 2.0 in ids


# ------------------------------------------------------------------ 逐像元

def test_con_condition(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "c", "op": "raster.con", "input": "@g",
         "condition": "value >= 20", "true": 2, "false": 1},
    ])
    rf = load_raster(r["result"]["path"])
    vals, counts = np.unique(rf.data[np.isfinite(rf.data)], return_counts=True)
    # 25 个 10 值像元 → 1；25 个 20 值像元 → 2；50 个 NoData 像元保持 NoData
    assert dict(zip(vals.tolist(), counts.tolist())) == {1.0: 25, 2.0: 25}
    assert int(np.isnan(rf.data).sum()) == 50


def test_con_nodata_condition(square, tmp_path):
    """显式 nodata 条件才处理 NoData：NoData→0 链路。"""
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "c", "op": "raster.con", "input": "@g",
         "condition": "nodata", "true": 0},
    ])
    rf = load_raster(r["result"]["path"])
    assert int(np.isnan(rf.data).sum()) == 0
    assert float(np.nanmin(rf.data)) == 0.0


def test_calc_whitelist(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "c", "op": "raster.calc", "inputs": {"a": "@g"},
         "expression": "clip(a * 2 + 1, 0, 100)"},
    ])
    rf = load_raster(r["result"]["path"])
    assert set(np.unique(rf.data[np.isfinite(rf.data)])) == {21.0, 41.0}

    with pytest.raises(GisError) as ei:
        _op(tmp_path, [
            {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0},
            {"id": "x", "op": "raster.calc", "inputs": {"a": "@g"},
             "expression": "__import__('os').system('echo 1')"},
        ])
    assert ei.value.code == "E_BAD_REQUEST"


def test_reclassify_breaks_chain(two_points, tmp_path):
    """breaks 只出断点表，reclassify 消费——频率曲线式两步流（距离场有连续值）。"""
    r = _op(tmp_path, [
        {"id": "d", "op": "raster.distance", "input": str(two_points), "cellsize": 100},
        {"id": "b", "op": "raster.breaks", "input": "@d", "classes": 3, "method": "quantile"},
    ])
    rows = r["steps"][1]["table"]["rows"]
    mapping = [[row[1], row[2], i + 1] for i, row in enumerate(rows)]
    assert len(mapping) == 3
    r2 = _op(tmp_path, [
        {"id": "d", "op": "raster.distance", "input": str(two_points), "cellsize": 100},
        {"id": "rc", "op": "raster.reclassify", "input": "@d", "mapping": mapping},
    ])
    rf = load_raster(r2["result"]["path"])
    vals = set(np.unique(rf.data[np.isfinite(rf.data)]).tolist())
    assert vals == {1.0, 2.0, 3.0}


def test_histogram_cumulative(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "h", "op": "raster.histogram", "input": "@g", "bins": 2},
    ])
    rows = r["steps"][1]["table"]["rows"]
    assert sum(row[2] for row in rows) == 50  # 50 个有效像元
    assert rows[-1][3] == pytest.approx(100.0)  # 累计 100%


def test_weighted_sum_normalization(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "w", "op": "raster.weighted_sum",
         "inputs": [{"ref": "@g", "weight": 3}, {"ref": "@g", "weight": 1}]},
    ])
    rf = load_raster(r["result"]["path"])
    # 两层相同 + 权重归一化 → 结果等于原值
    assert set(np.unique(rf.data[np.isfinite(rf.data)])) == {10.0, 20.0}


# ------------------------------------------------------------------- 邻域

def test_focal_mean_smoothing(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "f", "op": "raster.focal", "input": "@g", "stat": "mean", "kernel_size": 3},
    ])
    rf = load_raster(r["result"]["path"])
    # 20 值块内部（row2,col7）保持 20；两块共享角点 (5,5) 的窗口混合 10/20 → 中间值
    assert float(rf.data[2, 7]) == pytest.approx(20.0)
    assert 10.0 < float(rf.data[5, 5]) < 20.0


def test_zonal_stats_matches_truth(square, tmp_path):
    regions = tmp_path / "zones.geojson"
    gpd.GeoDataFrame(
        {"z": ["low", "high"]},
        geometry=[Polygon([(0, 0), (5, 0), (5, 5), (0, 5)]),
                  Polygon([(5, 5), (10, 5), (10, 10), (5, 10)])],
        crs="EPSG:32650").to_file(regions, driver="GeoJSON")
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "z", "op": "raster.zonal_stats", "regions": str(regions),
         "data": "@g", "stats": ["mean", "majority"]},
    ])
    # regions 顺序：low 全在 10 区、high 全在 20 区（all_touched 无跨界像元）
    assert r["preview"]["rows"][0][r["preview"]["columns"].index("raster_mean")] == pytest.approx(10.0)
    assert r["preview"]["rows"][1][r["preview"]["columns"].index("raster_mean")] == pytest.approx(20.0)


# ------------------------------------------------------------------- 转出

def test_polygonize_dissolve(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "p", "op": "raster.polygonize", "input": "@g", "field": "grade", "dissolve": True},
    ])
    assert r["result"]["geometry_types"] == ["Polygon"]
    assert r["result"]["count"] == 2  # 两个值 → 溶解成两个面
    # 面积对拍：1 像元=1m²，每块 25 个 5×5 像元 = 25m²
    import geopandas as gpd2
    out = gpd2.read_file(r["result"]["path"])
    areas = sorted(out.geometry.area.tolist())
    assert areas == pytest.approx([25.0, 25.0])


def test_contour_levels(two_points, tmp_path):
    """距离场（连续值）上提等值线。"""
    r = _op(tmp_path, [
        {"id": "d", "op": "raster.distance", "input": str(two_points), "cellsize": 100},
        {"id": "c", "op": "raster.contour", "input": "@d", "levels": [250.0]},
    ])
    assert r["result"]["geometry_types"] == ["LineString"]
    assert r["result"]["count"] >= 1


def test_sample_closure(square, tmp_path):
    pts = tmp_path / "probe.geojson"
    gpd.GeoDataFrame(
        geometry=[Point(2, 2), Point(7, 7), Point(20, 20)], crs="EPSG:32650"
    ).to_file(pts, driver="GeoJSON")
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
        {"id": "s", "op": "raster.sample", "raster": "@g", "points": str(pts)},
    ])
    cols = r["preview"]["columns"]
    vals = [row[cols.index("value")] for row in r["preview"]["rows"]]
    assert vals[0] == pytest.approx(10.0)
    assert vals[1] == pytest.approx(20.0)
    assert vals[2] is None  # 范围外 → None


# ------------------------------------------------------------------ 可视化/inspect

def test_inspect_and_visualize_raster(square, tmp_path):
    r = _op(tmp_path, [
        {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0, "field": "v"},
    ])
    tif = r["result"]["path"]
    info = api.inspect({"path": tif})
    assert info["kind"] == "raster"
    assert info["shape"] == [10, 10]

    v = api.visualize(tmp_path, {"source": tif, "title": "栅格测试图",
                                 "style": {"mode": "continuous", "cmap": "YlOrRd"}})
    assert Path(v["image_path"]).exists()
    v2 = api.visualize(tmp_path, {"source": r["artifact_id"],
                                  "style": {"mode": "categorical",
                                            "category_colors": {"10": "#1f77b4", "20": "#d62728"}}})
    assert Path(v2["image_path"]).exists()


def test_vector_op_rejects_raster(square, tmp_path):
    with pytest.raises(GisError) as ei:
        _op(tmp_path, [
            {"id": "g", "op": "raster.from_vector", "input": str(square), "cellsize": 1.0},
            {"id": "f", "op": "query.filter", "input": "@g", "where": "v >= 1"},
        ])
    assert "栅格" in ei.value.message
