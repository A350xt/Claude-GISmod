"""CLI / bridge 协议层端到端测试（node bridge 复用同一契约）。"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
SYNTH = REPO / "datasets" / "synthetic"


def run_cli(payload: dict | str) -> tuple[dict, int]:
    raw = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False)
    proc = subprocess.run(
        [sys.executable, "-m", "claude_gis", "run", "--workdir", str(REPO / ".tmp-cli")],
        input=raw.encode("utf-8"),
        capture_output=True,
        cwd=REPO,
    )
    env = json.loads(proc.stdout.decode("utf-8"))
    return env, proc.returncode


def _req(ops, workdir=None):
    return {"action": "analyze", "workdir": str(workdir), "operations": ops}


# ------------------------------------------------------------------ 正常链路

def test_inspect_envelope():
    env, code = run_cli({"action": "inspect",
                         "path": str(SYNTH / "hospitals.geojson")})
    assert code == 0 and env["ok"]
    r = env["result"]
    assert r["kind"] == "vector" and r["count"] == 12
    assert any(f["name"] == "beds" for f in r["fields"])
    assert r["crs"] == "EPSG:4326"
    assert set(r["capabilities"]) >= {"proximity"}


def test_analyze_chain_persists_artifacts(workdir):
    ops = [
        {"id": "f", "op": "query.filter",
         "input": str(SYNTH / "schools.geojson"), "where": "`type` == '小学'"},
        {"id": "z", "op": "zonal.summarize",
         "regions": str(SYNTH / "districts.geojson"), "data": "@f",
         "stats": ["count", "sum"], "field": "students"},
    ]
    env, code = run_cli(_req(ops, REPO / ".tmp-cli"))
    assert code == 0 and env["ok"], env
    r = env["result"]
    art_dir = REPO / ".tmp-cli" / "artifacts" / r["artifact_id"]
    assert (art_dir / "meta.json").exists()
    assert (art_dir / "plan.json").exists()
    assert (art_dir / "result.gpkg").exists()
    assert r["crs"]["analysis"].startswith("EPSG:")
    assert all(isinstance(s, str) and s for s in r["summary"])


def test_cross_call_artifact_chaining():
    first_ops = [{"id": "p", "op": "query.measure",
                  "input": str(SYNTH / "parcels.geojson"), "measures": ["area_sqkm"]}]
    env1, _ = run_cli(_req(first_ops, REPO / ".tmp-cli"))
    assert env1["ok"], env1
    art = env1["result"]["artifact_id"]
    follow = [{"id": "z", "op": "zonal.summarize",
               "regions": str(SYNTH / "districts.geojson"), "data": art,
               "stats": ["total_area_sqkm"]}]
    env2, code = run_cli(_req(follow, REPO / ".tmp-cli"))
    assert code == 0 and env2["ok"], env2

    import geopandas as gpd

    first = gpd.read_file(env1["result"]["result"]["path"], layer="result", engine="pyogrio")
    second = gpd.read_file(env2["result"]["result"]["path"], layer="result", engine="pyogrio")
    v1 = float(first["area_sqkm"].sum())
    v2 = float(second["data_area_sqkm"].sum())
    # 合成城区界存在收缩缝隙：跨区求和允许少量落在所有行政区之外（生成期已知特性）
    assert v1 * 0.85 <= v2 <= v1 * 1.0001, f"zonal 合计 {v2} 超出 [{v1*0.85:.3f},{v1:.3f}]"


def test_visualize_writes_png():
    pytest.importorskip("matplotlib")
    base_ops = [{"id": "m", "op": "query.measure",
                 "input": str(SYNTH / "parcels.geojson"), "measures": ["area_sqkm"]}]
    env1, _ = run_cli(_req(base_ops, REPO / ".tmp-cli"))
    assert env1["ok"], env1
    art = env1["result"]["artifact_id"]
    viz_req = {
        "action": "visualize", "workdir": str(REPO / ".tmp-cli"),
        "source": art, "style": {"mode": "choropleth", "field": "area_sqkm",
                                 "classification": "equal_interval", "classes": 3},
        "title": "地块面积分级图",
    }
    env, code = run_cli(viz_req)
    assert code == 0 and env["ok"], env
    img = Path(env["result"]["image_path"])
    assert img.exists() and img.stat().st_size > 10_000
    assert len(env["result"]["legend"]) >= 3


def test_show_action_returns_meta():
    env0, _ = run_cli(_req([{"id": "m", "op": "query.measure",
                             "input": str(SYNTH / "river.geojson"),
                             "measures": ["length_km"]}], REPO / ".tmp-cli"))
    art = env0["result"]["artifact_id"]
    env, code = run_cli({"action": "show", "workdir": str(REPO / ".tmp-cli"),
                         "artifact_id": art})
    assert code == 0 and env["ok"]
    assert env["result"]["artifact_id"] == art


# ------------------------------------------------------------------ 错误路径

def test_malformed_json_stdin_gives_envelope():
    env, code = run_cli("{not json")
    assert code == 2
    assert env["ok"] is False and env["error"]["code"] == "E_BAD_REQUEST"


def test_unknown_op_lists_vocabulary():
    ops = [{"id": "x", "op":"no.such"}]
    env, _ = run_cli(_req(ops, REPO / ".tmp-cli"))
    err = env["error"]
    assert err["code"] == "E_OP_UNKNOWN"
    assert "query.filter" in err["details"]["available"]


def test_missing_input_reports_missing_code():
    ops = [{"id": "x", "op": "query.measure", "input": "D:/definitely/not_here.geojson"}]
    env, _ = run_cli(_req(ops, REPO / ".tmp-cli"))
    assert env["error"]["code"] == "E_INPUT_MISSING"


def test_empty_step_requires_allow_empty():
    ops = [{"id": "x", "op": "query.filter",
            "input": str(SYNTH / "parcels.geojson"), "where": "pop_density > 9999999"}]
    env, _ = run_cli(_req(ops, REPO / ".tmp-cli"))
    assert env["error"]["code"] == "E_EMPTY_RESULT"
    assert "allow_empty" in env["error"]["message"]

    ops[0]["allow_empty"] = True
    env2, code = run_cli(_req(ops, REPO / ".tmp-cli"))
    assert code == 0 and env2["ok"]
    assert any("为空" in s for s in env2["result"]["summary"])


def test_bad_where_is_domain_error_not_crash():
    ops = [{"id": "w", "op": "query.filter",
            "input": str(SYNTH / "parcels.geojson"), "where": "TOTALLY ;; BAD"}]
    env, code = run_cli(_req(ops, REPO / ".tmp-cli"))
    assert env["ok"] is False and env["error"]["code"] == "E_BAD_REQUEST"
