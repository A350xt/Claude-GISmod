"""Overlay handlers."""

from __future__ import annotations

from claude_gis.analysis.common import StepResult, need
from claude_gis.providers.geostack import PROVIDER


def _both(resolver, params: dict, op: str):
    a, prov_a = resolver.frame(need(params, "a", op))
    b, _ = resolver.frame(need(params, "b", op))
    return a, b


def handle_intersection(resolver, params: dict, step_id: str) -> StepResult:
    a, b = _both(resolver, params, "overlay.intersection")
    out = PROVIDER.intersection(a, b)
    return StepResult(out, [f"叠加求交：结果 {len(out)} 个要素"])


def handle_difference(resolver, params: dict, step_id: str) -> StepResult:
    a, b = _both(resolver, params, "overlay.difference")
    out = PROVIDER.difference(a, b)
    return StepResult(
        out,
        [f"叠加扣除（A−B）：从 {len(a)} 个 A 要素得到 {len(out)} 个剩余部分"],
    )


def handle_union(resolver, params: dict, step_id: str) -> StepResult:
    a, b = _both(resolver, params, "overlay.union")
    out = PROVIDER.union(a, b)
    return StepResult(out, [f"叠加合并（A∪B）：结果 {len(out)} 个要素"])


def handle_clip(resolver, params: dict, step_id: str) -> StepResult:
    a, _ = resolver.frame(need(params, "a", "overlay.clip"))
    mask, _ = resolver.frame(need(params, "b", "overlay.clip"))
    out = PROVIDER.clip(a, mask)
    return StepResult(out, [f"按掩膜裁剪：保留 {len(out)} 个要素（原 {len(a)} 个）"])


SPEC = {
    "overlay.intersection": handle_intersection,
    "overlay.difference": handle_difference,
    "overlay.union": handle_union,
    "overlay.clip": handle_clip,
}
