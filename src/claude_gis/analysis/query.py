"""Query & Measure handlers."""

from __future__ import annotations

from claude_gis.analysis.common import StepResult, need, opt
from claude_gis.protocol import E_BAD_REQUEST, E_INPUT_MISSING, GisError, json_safe
from claude_gis.providers.geostack import PROVIDER

GROUP_TABLE_LIMIT = 50


def handle_filter(resolver, params: dict, step_id: str) -> StepResult:
    where = str(need(params, "where", "query.filter"))
    gdf, _ = resolver.frame(need(params, "input", "query.filter"))
    n_before = len(gdf)
    out = PROVIDER.filter_attributes(gdf, where)
    return StepResult(
        out,
        [f"属性过滤 `{where}`：{len(out)} 条命中（原 {n_before} 条）"],
    )


def handle_select(resolver, params: dict, step_id: str) -> StepResult:
    predicate = str(opt(params, "predicate", "intersects"))
    ref_tok = need(params, "ref", "query.select")
    gdf, _ = resolver.frame(need(params, "input", "query.select"))
    ref, _ = resolver.frame(ref_tok)
    n_before = len(gdf)
    out = PROVIDER.select_spatial(gdf, predicate, ref)
    return StepResult(out, [f"空间选择（{predicate} 参照层）命中 {len(out)} 条（原 {n_before} 条）"])


def handle_measure(resolver, params: dict, step_id: str) -> StepResult:
    measures = list(opt(params, "measures", ["count"]))
    group_by = opt(params, "group_by", None)
    gdf, _ = resolver.frame(need(params, "input", "query.measure"))
    out, totals = PROVIDER.measure(gdf, measures)
    parts = []
    if "area_sqkm_sum" in totals:
        parts.append(f"总面积 {totals['area_sqkm_sum']:.3f} km²")
        if "area_ha_sum" in totals:
            parts.append(f"{totals['area_ha_sum']:.1f} ha")
    if "length_km_sum" in totals:
        parts.append(f"总长度 {totals['length_km_sum']:.3f} km")
    if "count" in totals:
        parts.append(f"共 {int(totals['count'])} 条要素")
    summaries = ["度量结果：" + "；".join(parts)]

    table = None
    if group_by:
        group_by = str(group_by)
        if group_by not in out.columns:
            raise GisError(
                E_INPUT_MISSING,
                f"分组字段不存在：{group_by}",
                {"columns": [str(c) for c in out.columns]},
            )
        # provider 刚写入的度量列（length_km / area_sqkm / …_calc）
        value_cols = [c for c in out.columns
                      if c not in gdf.columns and str(c) != out.geometry.name]
        grouped = out.groupby(group_by, dropna=False)
        columns = [group_by, "count", *[str(c) for c in value_cols]]
        rows = []
        for key, part in grouped:
            row = [json_safe(key), int(len(part))]
            row.extend(float(part[c].sum()) for c in value_cols)
            rows.append(row)
        truncated = len(rows) > GROUP_TABLE_LIMIT
        if truncated:
            rows = rows[:GROUP_TABLE_LIMIT]
        table = {"columns": columns, "rows": rows}
        summaries.append(
            f"按 {group_by} 分组明细：{len(grouped)} 组"
            + (f"（表中仅前 {GROUP_TABLE_LIMIT} 组）" if truncated else "")
            + "——每组数值见 steps[].table"
        )

    return StepResult(out, summaries, table=table)


SPEC = {
    "query.filter": handle_filter,
    "query.select": handle_select,
    "query.measure": handle_measure,
}
