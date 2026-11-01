"""Plan executor: runs the ordered operation list and persists one artifact."""

from __future__ import annotations

import re

import numpy as np

from claude_gis.analysis import overlay, proximity, query, raster, trajectory, zonal
from claude_gis.analysis.common import preview_table
from claude_gis.analysis.resolver import DatasetResolver
from claude_gis.protocol import E_BAD_REQUEST, E_EMPTY_RESULT, E_OP_UNKNOWN, GisError
from claude_gis.raster_core import RasterFrame, _check_cells
from claude_gis.runtime.artifact import ArtifactStore

_STEP_ID_RE = re.compile(r"\A[A-Za-z0-9_]{1,32}\Z")

OP_HANDLERS: dict[str, object] = {}
for _mod in (query, proximity, overlay, zonal, trajectory, raster):
    OP_HANDLERS.update(_mod.SPEC)

# 不需要度量坐标系的操作：纯属性过滤 / 拓扑选择 / 轨迹排序。
# 全部步骤都属于此类时，默认保持源 CRS（不悄悄重投影到 UTM）。
NON_METRIC_OPS = {"query.filter", "query.select", "trajectory.build"}


def execute_analyze(workdir, request: dict) -> dict:
    operations = request.get("operations")
    if not isinstance(operations, list) or not operations:
        raise GisError(E_BAD_REQUEST, "analyze 需要 operations 列表（至少一步）")

    title = str(request.get("title") or "空间分析")
    output_format = str((request.get("output") or {}).get("format", "gpkg")).lower()

    # 请求级 CRS 覆盖：显式指定 > 自动策略（纯非度量管线保持源 CRS）
    crs_override = request.get("crs")
    if crs_override is not None and not isinstance(crs_override, str):
        raise GisError(E_BAD_REQUEST, "crs 必须是字符串（如 'EPSG:32622' 或 'source'）")
    if crs_override is None:
        op_names = {str(op.get("op") or "") for op in operations if isinstance(op, dict)}
        if op_names and op_names <= NON_METRIC_OPS:
            crs_override = "source"

    store = ArtifactStore(workdir)
    artifact_id, _dir = store.create("analysis", title)
    resolver = DatasetResolver(store, crs_override=crs_override)

    steps: list[dict] = []
    summaries: list[str] = []
    seen_ids: set[str] = set()
    final_gdf = None
    intermediates: dict[str, object] = {}

    for i, op_req in enumerate(operations):
        if not isinstance(op_req, dict):
            raise GisError(E_BAD_REQUEST, f"第 {i + 1} 步不是对象")
        op = op_req.get("op")
        if not op:
            raise GisError(E_BAD_REQUEST, f"第 {i + 1} 步缺少 op 字段")
        handler = OP_HANDLERS.get(op)
        if handler is None:
            raise GisError(
                E_OP_UNKNOWN,
                f"未知操作：{op}",
                {"step": i + 1, "available": sorted(OP_HANDLERS)},
            )
        raw_id = str(op_req.get("id") or f"s{i + 1}")
        if not _STEP_ID_RE.fullmatch(raw_id) or raw_id in seen_ids:
            raise GisError(E_BAD_REQUEST, f"步骤 id 非法或重复：{raw_id}", {"step": i + 1})
        seen_ids.add(raw_id)

        params = {k: v for k, v in op_req.items() if k not in ("op", "id", "allow_empty")}
        allow_empty = bool(op_req.get("allow_empty", False))
        try:
            result = handler(resolver, params, raw_id)
        except GisError as exc:
            exc.details = {**exc.details, "step": raw_id, "op": op}
            raise
        except KeyError as exc:
            raise GisError(E_BAD_REQUEST, str(exc.args[0]), {"step": raw_id, "op": op}) from exc

        gdf = result.gdf
        raster_rf = result.raster
        if raster_rf is not None:
            _check_cells(raster_rf)
            empty = raster_rf.is_empty()
            count = int(np.isfinite(raster_rf.data).sum())
        else:
            empty = gdf is None or len(gdf) == 0
            count = int(len(gdf)) if gdf is not None else 0
        if empty:
            if not allow_empty:
                raise GisError(
                    E_EMPTY_RESULT,
                    f"步骤 {raw_id}（{op}）结果为空；如空集属预期，请在该步加 \"allow_empty\": true",
                    {"step": raw_id, "op": op},
                )
            result.summaries.append(f"（步骤 {raw_id} 结果为空——已按 allow_empty 继续）")

        payload = raster_rf if raster_rf is not None else gdf
        resolver.register_step(raw_id, payload)
        intermediates[raw_id] = payload
        first_summary = result.summaries[0] if result.summaries else ""
        step_entry = {
            "id": raw_id,
            "op": op,
            "count": count,
            "summary": first_summary,
        }
        if result.table:
            step_entry["table"] = result.table
        steps.append(step_entry)
        summaries.extend(result.summaries)
        final_gdf = gdf
        final_raster = raster_rf

    meta = store.finalize_analysis(
        artifact_id,
        title=title,
        operations=operations,
        steps=steps,
        summaries=summaries,
        warnings=list(dict.fromkeys(resolver.warnings)),
        crs_info=resolver.crs_info(),
        inputs=resolver.inputs_meta,
        result_gdf=(None if final_raster is not None else final_gdf),
        result_raster=final_raster,
        result_format=output_format,
        intermediates=intermediates,
    )
    # 栅格结果的"预览表"由 steps[].table（分位/元信息）承担
    preview = preview_table(final_gdf) if (final_gdf is not None
                                           and final_raster is None) else None
    return {
        "artifact_id": artifact_id,
        "title": title,
        "result": meta["result"],
        "summary": summaries,
        "steps": steps,
        "preview": preview,
        "warnings": meta["warnings"],
        "crs": meta["crs"],
        "inputs": resolver.inputs_meta,
        "intermediate_layers": meta.get("intermediate_layers", {}),
    }
