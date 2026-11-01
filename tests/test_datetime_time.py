"""datetime 一等数据 + 时间类操作测试（实测反馈第 2/4/7 条）。"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd
import pandas as pd
import pytest
from shapely.geometry import Point

from geocore import api
from geocore.runtime.datasource import load_dataset


@pytest.fixture
def track_csv(tmp_path: Path) -> Path:
    """两条飓风轨迹的定位点（CSV，ISO 时间文本 + 经纬度）。"""
    rows = []
    # 风暴 A：3 个点自西向东
    for i, (lon, t) in enumerate([(120.0, "2024-09-01T06:00:00"),
                                  (120.5, "2024-09-01T12:00:00"),
                                  (121.1, "2024-09-01T18:00:00")]):
        rows.append({"storm": "A", "time": t, "wind_kt": 80 + i * 5,
                     "lon": lon, "lat": 28.0})
    # 风暴 B：2 个点自东向西（时间乱序写入，验证排序）
    for lon, t in [(122.0, "2024-09-02T12:00:00"), (121.4, "2024-09-02T00:00:00")]:
        rows.append({"storm": "B", "time": t, "wind_kt": 95, "lon": lon, "lat": 27.5})
    path = tmp_path / "tracks.csv"
    pd.DataFrame(rows).to_csv(path, index=False)
    return path


def test_csv_datetime_auto_parsed(track_csv: Path):
    gdf, prov = load_dataset(str(track_csv))
    assert pd.api.types.is_datetime64_any_dtype(gdf["time"])
    assert str(prov["format"]).startswith("csv")


def test_inspect_datetime_samples_serialize(track_csv: Path):
    """修复前：datetime 样本直接 `Timestamp is not JSON serializable` 崩掉。"""
    import json

    result = api.inspect({"path": str(track_csv)})
    time_field = next(f for f in result["fields"] if f["name"] == "time")
    assert time_field["dtype"].startswith("datetime64")
    for sample in time_field["samples"]:
        assert isinstance(sample, str) and sample.startswith("2024-09-0")
    json.dumps(result)  # 整包可序列化


def test_filter_by_time(track_csv: Path, tmp_path: Path):
    result = api.analyze(tmp_path, {
        "title": "9月1日午后的点",
        "operations": [
            {"id": "t", "op": "query.filter",
             "input": str(track_csv), "where": 'time >= "2024-09-01T07:00:00"'},
        ],
    })
    assert result["result"]["count"] == 4  # A 的 12:00/18:00 + B 的两点


def test_trajectory_build(track_csv: Path, tmp_path: Path):
    result = api.analyze(tmp_path, {
        "title": "风暴轨迹",
        "operations": [
            {"id": "tr", "op": "trajectory.build",
             "input": str(track_csv), "time_field": "time", "group_by": "storm"},
        ],
    })
    assert result["result"]["count"] == 2
    assert set(result["result"]["geometry_types"]) == {"LineString"}
    table = result["steps"][0]["table"]
    assert table["columns"][:2] == ["storm", "point_count"]
    by_storm = {r[0]: r for r in table["rows"]}
    assert by_storm["A"][1] == 3 and by_storm["B"][1] == 2
    # B 的时间乱序写入：start 必须是 00:00 那个点
    assert by_storm["B"][2] == "2024-09-02T00:00:00"


def test_measure_group_by_detail(track_csv: Path, tmp_path: Path):
    """修复前：measure 只有总量，分组明细要自己去读 artifact 的 gpkg。"""
    result = api.analyze(tmp_path, {
        "title": "按风暴统计",
        "operations": [
            {"id": "tr", "op": "trajectory.build",
             "input": str(track_csv), "time_field": "time", "group_by": "storm"},
            {"id": "m", "op": "query.measure",
             "input": "@tr", "measures": ["length_km", "count"], "group_by": "storm"},
        ],
    })
    table = result["steps"][1]["table"]
    assert table["columns"] == ["storm", "count", "length_km"]
    lengths = {r[0]: r[2] for r in table["rows"]}
    assert lengths["A"] > 0 and lengths["B"] > 0
    # A 的轨迹长度：3 点 2 段，(0.5°+0.6°)·111.32·cos(28°) ≈ 108 km
    assert lengths["A"] == pytest.approx(108.2, abs=3)


def test_preview_datetime_iso(track_csv: Path, tmp_path: Path):
    result = api.preview(tmp_path, {"source": str(track_csv)})
    time_field = next(f for f in result["meta"]["fields"] if f["name"] == "time")
    assert str(time_field["samples"][0]).startswith("2024-09-")
