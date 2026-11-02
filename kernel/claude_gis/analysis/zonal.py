"""Zonal statistics handler."""

from __future__ import annotations

from claude_gis.analysis.common import StepResult, need, opt
from claude_gis.providers.geostack import PROVIDER

_KIND_LABEL = {"point": "点要素", "line": "线要素", "polygon": "面要素"}


def handle_zonal(resolver, params: dict, step_id: str) -> StepResult:
    regions, _ = resolver.frame(need(params, "regions", "zonal.summarize"))
    data, _ = resolver.frame(need(params, "data", "zonal.summarize"))
    stats = list(opt(params, "stats", ["count"]))
    field = opt(params, "field", None)
    field = str(field) if field is not None else None

    out, info = PROVIDER.zonal_summarize(regions, data, stats, field)

    kind = info.get("kind", "")
    lines = [
        f"分区统计（{_KIND_LABEL.get(kind, kind)} × {len(out)} 个区域）："
        f"覆盖区域 {int((out['data_count'] > 0).sum())} 个，未覆盖 {int((out['data_count'] == 0).sum())} 个"
    ]
    if info.get("grand_total") is not None:
        lines.append(f"数据层共 {info['grand_total']} 条，其中落进任意区域的 {info['matched']} 条")
    if info.get("grand_total_length_km") is not None:
        hit = float(out["data_length_km"].sum()) if "data_length_km" in out.columns else 0.0
        lines.append(f"落入区域的线总长 {hit:.3f} km（全层 {info['grand_total_length_km']} km）")
    if info.get("grand_total_area_sqkm") is not None:
        hit = float(out["data_area_sqkm"].sum()) if "data_area_sqkm" in out.columns else 0.0
        lines.append(f"落入区域的面合计 {hit:.3f} km²（全层 {info['grand_total_area_sqkm']} km²）")
    return StepResult(out, lines)


SPEC = {
    "zonal.summarize": handle_zonal,
}
