"""中文/UTF-8 与 datetime 一等支持回归测试。

背景：中文 Windows 上 Python 子进程默认按 GBK 解码 stdin，中文参数/路径/标题
会乱码甚至报 surrogates 错误——这是实测反馈的头号硬伤。test_subprocess_utf8_roundtrip
直接走 `python -m claude_gis run` 子进程（不经 api.py 直调），锁死该修复。
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import geopandas as gpd
import pytest
from shapely.geometry import Point

from claude_gis import api


@pytest.fixture
def cjk_dataset(tmp_path: Path) -> Path:
    """中文目录 + 中文文件名 + 中文属性值的数据集。"""
    d = tmp_path / "实验数据" / "临江市"
    d.mkdir(parents=True)
    gdf = gpd.GeoDataFrame(
        {
            "名称": ["中心医院", "滨江学校", "东郊医院"],
            "类型": ["医院", "学校", "医院"],
            "床位": [800, 0, 300],
        },
        geometry=[Point(121.4, 31.2), Point(121.5, 31.25), Point(121.6, 31.3)],
        crs="EPSG:4326",
    )
    path = d / "设施.geojson"
    gdf.to_file(path, driver="GeoJSON")
    return path


def test_inspect_chinese_path_and_values(cjk_dataset: Path):
    result = api.inspect({"path": str(cjk_dataset)})
    names = [f["name"] for f in result["fields"]]
    assert "名称" in names and "类型" in names
    type_field = next(f for f in result["fields"] if f["name"] == "类型")
    assert set(type_field["samples"]) >= {"医院"}


def test_analyze_chinese_title_and_where(cjk_dataset: Path, tmp_path: Path):
    workdir = tmp_path / "工作目录"
    workdir.mkdir()
    result = api.analyze(workdir, {
        "title": "临江市医院筛选",
        "operations": [
            {"id": "h", "op": "query.filter",
             "input": str(cjk_dataset), "where": "类型 == '医院'"},
        ],
    })
    assert result["title"] == "临江市医院筛选"
    assert result["result"]["count"] == 2
    # 纯属性过滤：保持源 CRS，不悄悄重投影
    assert result["crs"]["analysis"] == "EPSG:4326"
    # preview 直接带回中文明细，Agent 不必再读 artifact
    assert result["preview"]["columns"] == ["名称", "类型", "床位"]
    assert result["preview"]["rows"][0][1] == "医院"


def test_subprocess_utf8_roundtrip(cjk_dataset: Path, tmp_path: Path):
    """端到端锁死 stdin/stdout UTF-8：中文路径 + 中文标题 + 中文 where 全链路。

    子进程继承本机 locale（中文 Windows 为 GBK）——修复前这里必然乱码/失败。
    """
    workdir = tmp_path / "工作目录"
    workdir.mkdir()
    request = {
        "action": "analyze",
        "title": "中文标题·端到端",
        "operations": [
            {"id": "f", "op": "query.filter",
             "input": str(cjk_dataset), "where": "类型 == '医院'"},
        ],
    }
    proc = subprocess.run(
        [sys.executable, "-m", "claude_gis", "run", "--workdir", str(workdir)],
        input=json.dumps(request, ensure_ascii=False).encode("utf-8"),
        capture_output=True,
        timeout=120,
    )
    assert proc.returncode == 0, proc.stderr.decode("utf-8", errors="replace")
    env = json.loads(proc.stdout.decode("utf-8"))
    assert env["ok"], env
    assert env["result"]["title"] == "中文标题·端到端"
    assert env["result"]["result"]["count"] == 2


def test_visualize_chinese_title(cjk_dataset: Path, tmp_path: Path):
    workdir = tmp_path / "工作目录"
    workdir.mkdir()
    result = api.visualize(workdir, {
        "source": str(cjk_dataset),
        "title": "临江市设施分布图",
        "style": {"mode": "categorical", "field": "类型",
                  "category_colors": {"医院": "#d62728", "学校": "#1f77b4"}},
    })
    assert Path(result["image_path"]).exists()
    assert "临江市设施分布图" in Path(result["image_path"]).name
