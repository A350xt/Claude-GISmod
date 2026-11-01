"""清单（inventory）范围与健壮性测试（实测反馈：坏 meta 整包崩 + 数据源越界）。"""

from __future__ import annotations

import json
from pathlib import Path

import geopandas as gpd
from shapely.geometry import Point

from claude_gis import api


def _pts(path: Path) -> Path:
    gdf = gpd.GeoDataFrame({"v": [1, 2]}, geometry=[Point(0, 0), Point(1, 1)], crs="EPSG:4326")
    gdf.to_file(path, driver="GeoJSON")
    return path


def test_corrupt_meta_skipped_not_fatal(tmp_path: Path):
    """0 字节 meta.json：清单跳过并计数，绝不再抛 JSONDecodeError。"""
    workdir = tmp_path / "w"
    workdir.mkdir()
    _pts(tmp_path / "pts.geojson")
    api.analyze(workdir, {
        "title": "正常产物",
        "operations": [{"id": "f", "op": "query.filter",
                        "input": str(tmp_path / "pts.geojson"), "where": "v >= 1"}],
    })
    ids = sorted((workdir / "artifacts").iterdir())
    (ids[0] / "meta.json").write_text("", encoding="utf-8")

    result = api.list_inventory(workdir, {})
    assert result["skipped_corrupt"] == 1
    assert len(result["artifacts"]) == 0          # 唯一产物坏了 → 跳过
    assert not any("JSONDecodeError" in str(a) for a in result["artifacts"])


def test_agent_used_inputs_listed_first(tmp_path: Path):
    """agent 用过的数据集出现在清单里（origin=agent-used），排在工作区扫描之前。"""
    workdir = tmp_path / "w"
    data = _pts(tmp_path / "used_by_agent.geojson")
    _pts(tmp_path / "never_touched.geojson")
    api.analyze(workdir, {
        "title": "用过 used_by_agent",
        "operations": [{"id": "f", "op": "query.filter",
                        "input": str(data), "where": "v >= 1"}],
    })

    result = api.list_inventory(workdir, {"datasets_dirs": [str(tmp_path)]})
    origins = {d["name"]: d["origin"] for d in result["datasets"]}
    assert origins["used_by_agent"] == "agent-used"
    assert origins["never_touched"] == "workspace"
    assert result["datasets"][0]["origin"] == "agent-used"   # agent 用过的排最前


def test_workspace_scan_skips_junk_and_is_capped(tmp_path: Path):
    workdir = tmp_path / "w"
    _pts(tmp_path / "real.geojson")
    (tmp_path / "node_modules").mkdir()
    _pts(tmp_path / "node_modules" / "dep.geojson")
    deep = tmp_path / "a" / "b" / "c" / "d"
    deep.mkdir(parents=True)
    _pts(deep / "too_deep.geojson")

    result = api.list_inventory(workdir, {"datasets_dirs": [str(tmp_path)]})
    names = [d["name"] for d in result["datasets"]]
    assert "real" in names
    assert "dep" not in names        # node_modules 被跳过
    assert "too_deep" not in names   # 超过深度 3


def test_no_roots_means_agent_driven_only(tmp_path: Path):
    """不给 datasets_dirs：清单只含 agent 用过的文件 + artifacts（最小可见面）。"""
    workdir = tmp_path / "w"
    data = _pts(tmp_path / "only_this.geojson")
    _pts(tmp_path / "invisible.geojson")
    api.analyze(workdir, {
        "title": "t",
        "operations": [{"id": "f", "op": "query.filter",
                        "input": str(data), "where": "v >= 1"}],
    })

    result = api.list_inventory(workdir, {})
    names = [d["name"] for d in result["datasets"]]
    assert names == ["only_this"]
    assert len(result["artifacts"]) == 1
    # 兼容旧参数名
    legacy = api.list_inventory(workdir, {"datasets_dir": str(tmp_path)})
    assert {d["name"] for d in legacy["datasets"]} >= {"only_this", "invisible"}


def test_atomic_meta_write(tmp_path: Path):
    """meta.json 落盘后目录里不应残留 .tmp 中间文件。"""
    workdir = tmp_path / "w"
    _pts(tmp_path / "pts.geojson")
    api.analyze(workdir, {
        "title": "原子写",
        "operations": [{"id": "f", "op": "query.filter",
                        "input": str(tmp_path / "pts.geojson"), "where": "v >= 1"}],
    })
    art_dir = next((workdir / "artifacts").iterdir())
    assert not list(art_dir.glob("*.tmp"))
    assert json.loads((art_dir / "meta.json").read_text(encoding="utf-8"))["title"] == "原子写"
