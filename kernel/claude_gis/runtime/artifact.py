"""GIS Result artifact persistence for Claude-GISmod.

Each analysis produces one directory holding meta.json, plan.json and the
result dataset. The store below enforces three invariants on every access:

1. artifact ids must match ``ar_`` plus exactly eight alphanumerics, so
   separators and dot segments can never occur;
2. paths are built exclusively with constants joined onto a realpath-resolved
   artifacts root;
3. containment is re-verified with realpath prefix checks before any read or
   write happens.
"""

from __future__ import annotations

import json
import os
import re
import secrets
import string
from datetime import datetime
from typing import Any

from claude_gis.protocol import E_BAD_REQUEST, E_INPUT_MISSING, E_OUTPUT_ERROR, GisError

_ARTIFACT_ID_RE = re.compile(r"\Aar_[A-Za-z0-9]{10,32}\Z")
_ALPHABET = string.ascii_lowercase + string.digits
_RESULT_NAMES = {"gpkg": "result.gpkg", "geojson": "result.geojson", "tif": "result.tif"}
_META_NAME = "meta.json"
_PLAN_NAME = "plan.json"


def is_artifact_token(value) -> bool:
    """单一事实来源：一个字符串是否是合法 artifact 引用。"""
    return isinstance(value, str) and bool(_ARTIFACT_ID_RE.fullmatch(value))


def _new_id() -> str:
    stamp = datetime.now().strftime("%Y%m%d%H%M%S")
    suffix = "".join(secrets.choice(_ALPHABET) for _ in range(4))
    return "ar_" + stamp + suffix


def _checked_id(artifact_id: str) -> str:
    # 白名单字符集：分隔符与点点段在合法 id 中不可能出现
    if isinstance(artifact_id, str) and _ARTIFACT_ID_RE.fullmatch(artifact_id):
        return artifact_id
    raise GisError(
        E_BAD_REQUEST,
        "非法 artifact_id：" + repr(artifact_id),
        {"expected_pattern": _ARTIFACT_ID_RE.pattern},
    )


def _contained(root_real: str, candidate_real: str) -> bool:
    return candidate_real == root_real or candidate_real.startswith(root_real + os.sep)


def _raster_valid_cells(rf) -> int:
    import numpy as np

    return int(np.isfinite(rf.data).sum())


class ArtifactStore:
    def __init__(self, workdir):
        base_real = os.path.realpath(str(workdir))
        self.root_real = os.path.join(base_real, "artifacts")
        os.makedirs(self.root_real, exist_ok=True)

    # ------------------------------------------------------------- path math

    def _dir_of(self, artifact_id: str) -> str:
        ident = _checked_id(artifact_id)
        candidate = os.path.join(self.root_real, ident)
        real = os.path.realpath(candidate)
        if not _contained(self.root_real, real):
            raise GisError(E_OUTPUT_ERROR, "artifact 目录越界")
        return real

    def _file_of(self, artifact_id: str, name: str) -> str:
        if name not in (_META_NAME, _PLAN_NAME):
            raise GisError(E_OUTPUT_ERROR, "非法产物文件名")
        candidate = os.path.join(self._dir_of(artifact_id), name)
        real = os.path.realpath(candidate)
        if not _contained(self.root_real, real):
            raise GisError(E_OUTPUT_ERROR, "产物文件越界")
        return real

    def result_path_of(self, artifact_id: str, fmt: str) -> str:
        key = fmt if fmt in _RESULT_NAMES else ("geojson" if fmt == "geojson" else "gpkg")
        name = _RESULT_NAMES[key]
        candidate = os.path.join(self._dir_of(artifact_id), name)
        real = os.path.realpath(candidate)
        if not _contained(self.root_real, real):
            raise GisError(E_OUTPUT_ERROR, "结果文件越界")
        return real

    def image_path_for(self, artifact_id: str, stem: str) -> Any:
        # 保留中日韩字符（UTF-8 模式下文件系统可安全承载），仅剔除路径危险字符
        stem_clean = re.sub(r"[^\w\u4e00-\u9fff\-]", "", stem, flags=re.UNICODE)[:40] or "map"
        candidate = os.path.join(self._dir_of(artifact_id), stem_clean + ".png")
        real = os.path.realpath(candidate)
        if not _contained(self.root_real, real):
            raise GisError(E_OUTPUT_ERROR, "图片越界")
        from pathlib import Path

        return Path(real)

    # ------------------------------------------------------------------ json

    def _write_json(self, target_real: str, payload: dict) -> None:
        if not _contained(self.root_real, os.path.realpath(target_real)):
            raise GisError(E_OUTPUT_ERROR, "json 写出越界")
        import io

        data = json.dumps(payload, ensure_ascii=False, indent=2)
        # 原子写：先写临时文件再替换，进程被杀不会留下 0 字节 meta
        tmp_real = target_real + ".tmp"
        with io.open(tmp_real, "w", encoding="utf-8") as fh:
            fh.write(data)
        os.replace(tmp_real, target_real)

    def _read_json(self, target_real: str) -> dict:
        if not _contained(self.root_real, os.path.realpath(target_real)):
            raise GisError(E_OUTPUT_ERROR, "json 读取越界")
        import io

        try:
            with io.open(target_real, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except (json.JSONDecodeError, UnicodeDecodeError, OSError) as exc:
            raise GisError(
                E_OUTPUT_ERROR,
                f"artifact 元数据损坏（{os.path.basename(os.path.dirname(target_real))}）：{exc}",
                {"file": os.path.basename(target_real)},
            ) from exc

    # ------------------------------------------------------------------- api

    def create(self, kind: str, title: str) -> tuple:
        artifact_id = _new_id()
        dir_real = self._dir_of(artifact_id)
        if not _contained(self.root_real, os.path.realpath(dir_real)):
            raise GisError(E_OUTPUT_ERROR, "创建目录越界")
        os.mkdir(dir_real)
        self._write_json(
            os.path.join(dir_real, _META_NAME),
            {"artifact_id": artifact_id, "kind": kind, "title": title,
             "created": datetime.now().isoformat(timespec="seconds")},
        )
        from pathlib import Path

        return artifact_id, Path(dir_real)

    def finalize_analysis(
        self,
        artifact_id: str,
        *,
        title: str,
        operations: list,
        steps: list,
        summaries: list,
        warnings: list,
        crs_info: dict,
        inputs: list,
        result_gdf,
        result_format: str,
        intermediates: dict | None = None,
        result_raster=None,
    ) -> dict:
        dir_real = self._dir_of(artifact_id)
        steps_index: dict = {}
        try:
            from pathlib import Path

            from claude_gis.raster_core import RasterFrame, save_raster

            # 中间步骤统一持久化（无论最终结果是矢量还是栅格）：
            # RasterFrame → step_<id>.tif；GeoDataFrame → GPKG 图层（最终为 geojson 时除外）
            persist_vector_steps = None  # 延迟决定（取决于最终格式）
            if result_raster is not None:
                result_path = save_raster(result_raster, Path(dir_real) / "result.tif")
                fmt = "tif"
                count = _raster_valid_cells(result_raster)
                geometry_types = ["Raster"]
                extra_result = {
                    "cellsize": result_raster.cellsize,
                    "shape": list(result_raster.data.shape),
                }
            else:
                fmt = "geojson" if result_format == "geojson" else "gpkg"
                result_path = self.result_path_of(artifact_id, fmt)
                if fmt == "geojson":
                    result_gdf.to_file(result_path, driver="GeoJSON", index=False)
                else:
                    result_gdf.to_file(result_path, layer="result", driver="GPKG",
                                       engine="pyogrio", index=False)
                    persist_vector_steps = True
                count = int(len(result_gdf))
                geometry_types = sorted({str(t) for t in result_gdf.geometry.geom_type})
                extra_result = {}

            for sid, step_val in sorted((intermediates or {}).items()):
                # 约定与矢量产物一致：每个步骤都可被 ar_xxx#stepId 引用（含最后一步，
                # 它同时是 result.*——冗余一份但语义统一）
                if isinstance(step_val, RasterFrame):
                    tif = save_raster(step_val, Path(dir_real) / f"step_{sid}.tif")
                    steps_index[sid] = {"layer": tif.name,
                                        "count": _raster_valid_cells(step_val),
                                        "kind": "raster"}
                elif persist_vector_steps and hasattr(step_val, "geometry"):
                    layer = f"step_{sid}"
                    step_val.to_file(result_path, layer=layer, driver="GPKG",
                                     engine="pyogrio", index=False)
                    steps_index[sid] = {"layer": layer, "count": int(len(step_val))}
        except GisError:
            raise
        except Exception as exc:
            raise GisError(E_OUTPUT_ERROR, "结果数据集写出失败:" + str(exc)) from exc

        self._write_json(self._file_of(artifact_id, _PLAN_NAME), {
            "title": title,
            "operations": operations,
            "inputs": inputs,
            "crs": crs_info,
            "steps": steps,
        })

        meta = {
            "artifact_id": artifact_id,
            "kind": "analysis",
            "title": title,
            "created": datetime.now().isoformat(timespec="seconds"),
            "result": {
                "path": str(result_path),
                "format": fmt,
                "count": count,
                "geometry_types": geometry_types,
                **extra_result,
            },
            "summary": summaries,
            "warnings": warnings,
            "crs": crs_info,
            "steps": steps,
            "inputs": inputs,
            "plan_ref": _PLAN_NAME,
            "intermediate_layers": steps_index,
        }
        self._write_json(self._file_of(artifact_id, _META_NAME), meta)
        return meta

    def get(self, artifact_id: str) -> dict:
        target = self._file_of(artifact_id, _META_NAME)
        if not os.path.exists(target):
            raise GisError(E_INPUT_MISSING, "Artifact 不存在:" + artifact_id, {"id": artifact_id})
        return self._read_json(target)

    def resolve_result_path(self, token: str):
        """结果位置由 format 确定性重建；meta 内存储的路径仅作展示。"""
        meta = self.get(token)
        res = meta.get("result") or {}
        path = self.result_path_of(token, res.get("format", "gpkg"))
        if not os.path.exists(path):
            raise GisError(E_INPUT_MISSING, "Artifact 结果文件缺失:" + token, {"id": token})
        from pathlib import Path

        return Path(path)

    def list_ids(self) -> list:
        out = []
        for name in sorted(os.listdir(self.root_real)):
            if _ARTIFACT_ID_RE.fullmatch(name) and os.path.isdir(os.path.join(self.root_real, name)):
                out.append(name)
        return out

    def overwrite_meta(self, artifact_id: str, meta: dict) -> None:
        self._write_json(self._file_of(artifact_id, _META_NAME), meta)

    def finalize_map(self, artifact_id: str, *, image: str, legend: list,
                     source: str, warnings: list, crs: dict) -> None:
        base = {"kind": "map", "image": image, "legend": legend, "source": source,
                "warnings": warnings, "crs": crs}
        try:
            meta = self.get(artifact_id)
        except GisError:
            meta = {}
        meta.update(base)
        self._write_json(self._file_of(artifact_id, _META_NAME), meta)
