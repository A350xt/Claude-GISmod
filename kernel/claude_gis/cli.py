"""claude_gis CLI：stdin 一条 JSON 请求 → stdout 一条 JSON 封套。

    echo '{"action":"inspect","path":"data/x.geojson"}' | python -m claude_gis run --workdir .

Claude Code mod（hooks/kernel.ts）就是通过它驱动内核的；任何宿主都可以用同一协议。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import traceback
from pathlib import Path
from typing import Callable

ACTIONS = ("inspect", "analyze", "visualize", "compose", "show", "operations", "preview", "list")


def _handlers() -> dict[str, Callable[[Path, dict], dict]]:
    from claude_gis import api

    return {
        "inspect": lambda wd, p: api.inspect(p),
        "analyze": api.analyze,
        "visualize": api.visualize,
        "compose": api.compose,
        "show": lambda wd, p: api.show(wd, p.get("artifact_id")),
        "operations": lambda wd, p: api.list_operations(),
        "preview": api.preview,
        "list": api.list_inventory,
    }


def _emit(envelope: dict) -> None:
    from claude_gis.protocol import json_default

    print(json.dumps(envelope, ensure_ascii=False, default=json_default))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="claude_gis")
    parser.add_argument("command", choices=["run"], help="执行一条 JSON 请求")
    parser.add_argument("--workdir", default=os.environ.get("CLAUDE_GIS_WORKDIR"))
    args = parser.parse_args(argv)

    # 中文 Windows 默认按 GBK 收发；协议统一 UTF-8
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")  # type: ignore[union-attr]
        except Exception:
            pass

    from claude_gis.protocol import envelope_from_exception, error_envelope, ok_envelope

    raw = sys.stdin.read()
    try:
        payload = json.loads(raw) if raw.strip() else {}
    except json.JSONDecodeError as exc:
        _emit(error_envelope("E_BAD_REQUEST", f"请求不是合法 JSON：{exc}"))
        return 2
    if not isinstance(payload, dict):
        _emit(error_envelope("E_BAD_REQUEST", "请求必须是 JSON 对象"))
        return 2

    action = str(payload.pop("action", ""))
    card_opts = payload.pop("_card", None)
    workdir = Path(payload.pop("workdir", None) or args.workdir or ".")
    handler = _handlers().get(action)
    if handler is None:
        _emit(error_envelope("E_BAD_REQUEST", f"未知 action：{action}",
                             {"available": list(ACTIONS)}))
        return 2

    try:
        request = dict(payload)
        result = handler(workdir, payload)
        envelope = ok_envelope(result)
        if isinstance(card_opts, dict):
            # 卡片只给界面用，失败不影响主结果
            from claude_gis import api

            try:
                envelope["card"] = api.card(workdir, action, request, result, card_opts)
            except Exception as exc:  # noqa: BLE001
                envelope["card"] = {"error": f"{type(exc).__name__}: {exc}"}
        _emit(envelope)
        return 0
    except Exception as exc:  # 统一出口：调用方永远收到封套
        env = envelope_from_exception(exc)
        if env["error"]["code"] == "E_INTERNAL":
            env["error"]["details"]["traceback"] = traceback.format_exc(limit=6)
        _emit(env)
        return 1 if env["error"]["code"] == "E_INTERNAL" else 2
