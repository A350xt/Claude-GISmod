"""Wire protocol shared by the CLI, the Claude Code mod and the tests.

Every command emits a single JSON envelope on stdout:

    {"ok": true,  "result": {...}}
    {"ok": false, "error": {"code": "...", "message": "...", "details": {...}}}

This module is the single source of truth for error codes.
"""

from __future__ import annotations

import datetime as _dt
from typing import Any

# Error codes (stable contract, see docs/tool-api.md)
E_BAD_REQUEST = "E_BAD_REQUEST"                    # malformed request / invalid params
E_INPUT_MISSING = "E_INPUT_MISSING"                # dataset path or artifact id not found
E_CRS_AMBIGUOUS = "E_CRS_AMBIGUOUS"                # missing CRS that cannot be assumed safely
E_OP_UNKNOWN = "E_OP_UNKNOWN"                      # operation not in the primitive vocabulary
E_GEOMETRY_INVALID_UNFIXABLE = "E_GEOMETRY_INVALID_UNFIXABLE"  # make_valid failed
E_EMPTY_RESULT = "E_EMPTY_RESULT"                  # an operation produced zero features
E_OUTPUT_ERROR = "E_OUTPUT_ERROR"                  # failed writing artifact/map
E_INTERNAL = "E_INTERNAL"                          # unexpected failure


class GeoCoreError(Exception):
    """Domain error carrying a stable wire code."""

    def __init__(self, code: str, message: str, details: dict[str, Any] | None = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details or {}


def json_safe(value: Any) -> Any:
    """递归转成可 json.dumps 的值：datetime→ISO 字符串、numpy 标量→Python 标量。

    时间字段是一等数据，任何响应出口（inspect samples / preview 表 / summaries）
    都必须先经它清洗，杜绝 `Timestamp is not JSON serializable`。
    """
    if value is None or isinstance(value, (bool, str, int)):
        return value
    if isinstance(value, float):
        return None if value != value else value  # NaN/Inf → None
    if isinstance(value, (_dt.datetime, _dt.date, _dt.time)):
        return value.isoformat()
    if isinstance(value, _dt.timedelta):
        return value.total_seconds()
    try:                        # numpy.datetime64：先于 .item() 分支，否则变纳秒整数
        import numpy as _np

        if isinstance(value, _np.datetime64):
            import pandas as _pd

            return _pd.Timestamp(value).isoformat()
    except ImportError:
        pass
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set, frozenset)):
        return [json_safe(v) for v in (sorted(value) if isinstance(value, (set, frozenset)) and
                                       all(isinstance(x, (str, int, float)) for x in value) else value)]
    if hasattr(value, "isoformat"):  # pandas.Timestamp
        try:
            return value.isoformat()
        except Exception:
            pass
    if hasattr(value, "item"):       # numpy 标量
        try:
            return json_safe(value.item())
        except Exception:
            pass
    return str(value)


def json_default(value: Any) -> Any:
    """json.dumps(default=...) 出口：漏网的类型按安全规则兜底，而不是抛异常。"""
    return json_safe(value)


def ok_envelope(result: dict[str, Any]) -> dict[str, Any]:
    return {"ok": True, "result": result}


def error_envelope(code: str, message: str, details: dict[str, Any] | None = None) -> dict[str, Any]:
    return {"ok": False, "error": {"code": code, "message": message, "details": details or {}}}


def envelope_from_exception(exc: Exception) -> dict[str, Any]:
    if isinstance(exc, GeoCoreError):
        return error_envelope(exc.code, exc.message, exc.details)
    return error_envelope(E_INTERNAL, f"{type(exc).__name__}: {exc}")
