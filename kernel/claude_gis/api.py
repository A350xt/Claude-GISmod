"""Public API used by the CLI (`python -m claude_gis run`)."""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd

from claude_gis.analysis.executor import OP_HANDLERS, execute_analyze
from claude_gis.protocol import (
    E_BAD_REQUEST,
    E_INPUT_MISSING,
    E_OUTPUT_ERROR,
    GisError,
    json_safe,
)
from claude_gis.runtime.artifact import ArtifactStore
from claude_gis.runtime.datasource import load_dataset
from claude_gis.runtime.prepare import assume_missing_crs


def inspect(payload: dict) -> dict:
    """Understand a dataset without analyzing it."""
    path = str(payload.get("path") or "")
    if Path(path).suffix.lower() in (".tif", ".tiff"):
        from claude_gis.raster_core import raster_meta_of

        meta = raster_meta_of(path)
        return {
            **meta,
            "source": {"format": "tif", "path": path},
            "capabilities": ["raster.*"],
            "supported_formats_note": "栅格已支持（raster 域算子）；多波段先指定 band",
            "notes": [
                f"NoData 占比 {meta['nodata_pct']}%",
                "栅格化/重采样/统计请走 analyze 工具的 raster.* 词表",
            ],
        }
    gdf, prov = load_dataset(path)

    types = {}
    for t in map(str, gdf.geometry.geom_type):
        types[t] = types.get(t, 0) + 1
    minx, miny, maxx, maxy = [float(v) for v in gdf.total_bounds]
    n_invalid = int((~gdf.geometry.is_valid).sum()) if len(gdf) else 0
    n_null = int(gdf.geometry.isna().sum())

    fields = []
    skip = {"geometry"}
    for col in gdf.columns:
        if col in skip:
            continue
        series = gdf[col]
        samples = [json_safe(v) for v in pd.unique(series.dropna())[:3]]
        fields.append({
            "name": str(col),
            "dtype": str(series.dtype),
            "samples": samples,
        })

    crs = gdf.crs.to_string() if gdf.crs is not None else None
    coords_in_range = bool(-181 <= minx <= 181 and -91 <= miny <= 91 and -181 <= maxx <= 181 and -91 <= maxy <= 91)
    warnings = []
    if crs is None:
        warnings.append("数据缺失 CRS" + ("（坐标在经纬度范围内，分析时可安全假定 EPSG:4326）" if coords_in_range
                                        else "且坐标不在经纬度范围内，度量前必须先明确 CRS"))
    if n_invalid:
        warnings.append(f"{n_invalid} 个无效几何，分析时将自动修复")
    if n_null:
        warnings.append(f"{n_null} 条要素缺少几何")

    return {
        "kind": "vector",
        "source": prov,
        "geometry_types": types,
        "count": int(len(gdf)),
        "crs": crs,
        "extent_bbox": [minx, miny, maxx, maxy],
        "fields": fields,
        "quality": {
            "invalid_geometries": n_invalid,
            "null_geometries": n_null,
            "missing_crs": crs is None,
            "wgs84_assumable_if_missing": coords_in_range,
        },
        "capabilities": ["query_measure", "proximity", "overlay", "zonal"],
        "supported_formats_note": "Phase 1 为矢量核心；Field/Network/Spatial Statistics 属于后续阶段",
        "notes": warnings,
    }


def analyze(workdir: Path, payload: dict) -> dict:
    return execute_analyze(Path(workdir), payload)


def visualize(workdir: Path, payload: dict) -> dict:
    from claude_gis.analysis.resolver import DatasetResolver
    from claude_gis.raster_core import is_raster_path
    from claude_gis.runtime.artifact import is_artifact_token
    from claude_gis.viz.map import render_map

    source = str(payload.get("source") or "")
    style = dict(payload.get("style") or {})
    title = str(payload.get("title") or style.get("title") or "GIS 地图")
    base = payload.get("base")
    base_style = dict(payload.get("base_style") or {}) if base else None

    store = ArtifactStore(Path(workdir))

    base_artifact = source if is_artifact_token(source) else None
    if base_artifact:
        # 校验存在性并让地图写入既有分析产物目录
        store.get(base_artifact)
        artifact_id = base_artifact
    else:
        artifact_id, _ = store.create("map", title)

    # 制图坐标系：缺省保持源 CRS；"utm" 交给自动选择（地理坐标 → UTM）；也可给 EPSG 串。
    # 多层（base）始终对齐到同一坐标系
    crs_req = str(payload.get("crs") or "source").strip()
    resolver = DatasetResolver(store, crs_override=None if crs_req.lower() == "utm" else crs_req)
    base_gdf = None
    if base:
        base_gdf, _ = resolver.frame(str(base))

    out_path = store.image_path_for(artifact_id, title)

    if _is_raster_source(store, source):
        # 栅格制图：连续色带拉伸 / 分类色表
        from claude_gis.viz.map import render_raster

        rf, _ = resolver.raster(source)
        legend = render_raster(rf, style=style, out_path=out_path, title=title)
    else:
        gdf, _ = resolver.frame(source)
        legend = render_map(gdf, style=style, out_path=out_path, title=title,
                            base_gdf=base_gdf, base_style=base_style)

    if not base_artifact:
        store.finalize_map(artifact_id, image=str(out_path), legend=legend,
                           source=source, warnings=list(dict.fromkeys(resolver.warnings)),
                           crs=resolver.crs_info())
    else:
        meta = store.get(artifact_id)
        meta.setdefault("images", [])
        if not any(img.get("path") == str(out_path) for img in meta["images"]):
            meta["images"].append({"path": str(out_path)})
        store.overwrite_meta(artifact_id, meta)
    return {
        "artifact_id": artifact_id,
        "image_path": str(out_path),
        "legend": legend,
        "warnings": list(dict.fromkeys(resolver.warnings)),
    }


def compose(workdir: Path, payload: dict) -> dict:
    """版面合成：多图层 + 图例/比例尺/指北针的成品版面 PNG。"""
    from claude_gis.viz.compose import render_composition

    spec = payload.get("spec") if isinstance(payload.get("spec"), dict) else payload
    title = str(spec.get("title") or payload.get("title") or "制图")
    dpi = int(payload.get("dpi", 200))
    dpi = max(36, min(dpi, 400))

    store = ArtifactStore(Path(workdir))
    artifact_id, _ = store.create("map", title)
    out_path = store.image_path_for(artifact_id, "layout")
    res = render_composition(store, spec, out_path=out_path, dpi=dpi)
    store.finalize_map(artifact_id, image=str(out_path), legend=[],
                       source="layout", warnings=res["warnings"],
                       crs={"analysis": "", "sources": {}})
    return {
        "artifact_id": artifact_id,
        "image_path": str(out_path),
        "warnings": list(dict.fromkeys(res["warnings"])),
    }


def show(workdir: Path, artifact_id: str) -> dict:
    store = ArtifactStore(Path(workdir))
    meta = store.get(str(artifact_id))
    listed = store.list_ids()
    if meta["artifact_id"] not in listed:
        raise GisError(E_INPUT_MISSING, "Artifact 索引异常", {"id": artifact_id})
    return meta


def list_operations() -> dict:
    """供 Agent 自省当前可用的操作词表。"""
    return {"operations": sorted(OP_HANDLERS)}


# --------------------------------------------------------------- 面板支持


def _is_raster_source(store: ArtifactStore, tok: str) -> bool:
    """来源是否为栅格：.tif 路径 / 栅格产物 / 栅格中间步骤。"""
    from claude_gis.analysis.resolver import parse_step_ref
    from claude_gis.raster_core import is_raster_path
    from claude_gis.runtime.artifact import is_artifact_token

    if is_raster_path(tok):
        return True
    if is_artifact_token(tok):
        meta = store.get(tok)
        return str((meta.get("result") or {}).get("format", "")).lower() in ("tif", "tiff")
    step_ref = parse_step_ref(tok)
    if step_ref:
        meta = store.get(step_ref[0])
        info = (meta.get("intermediate_layers") or {}).get(step_ref[1]) or {}
        return str(info.get("kind", "")) == "raster" or             str(info.get("layer", "")).lower().endswith(".tif")
    return False


def preview(workdir: Path, payload: dict) -> dict:
    """面板缩略图：数据集路径 / artifact_id / artifact_id#步骤 → 地图卡片 SVG（+ 可选 PNG）与摘要。

    请求：{source, crs?: "source"|"utm"|"EPSG:xxxx", png?: bool}。
    crs 只影响显示（重投影后出图、范围按该坐标系给出），不改动任何数据。
    """
    import base64

    import geopandas as gpd
    from shapely.geometry import box

    from claude_gis.analysis.resolver import DatasetResolver
    from claude_gis.viz import preview as pv

    source = str(payload.get("source") or "")
    if not source:
        raise GisError(E_BAD_REQUEST, "preview 需要 source（路径或 artifact_id）")
    want_png = bool(payload.get("png"))
    crs_req = str(payload.get("crs") or "source")
    store = ArtifactStore(Path(workdir))
    resolver = DatasetResolver(store, crs_override="source")
    title = Path(source).stem if not source.startswith("ar_") else source

    if _is_raster_source(store, source):
        rf, _ = resolver.raster(source)
        source_crs = pv.resolve_display_crs(rf.crs, "source")
        footprint = gpd.GeoDataFrame(geometry=[box(*rf.bounds)], crs=rf.crs)
        target = pv.resolve_display_crs(rf.crs, crs_req, footprint)
        rf = pv.reproject_raster(rf, target)
        svg, png = pv.raster_svg(rf, title)
        m = rf.meta()
        out = {
            "kind": "raster",
            "meta": {
                "shape": m["shape"], "cellsize": m["cellsize"],
                "nodata_pct": m["nodata_pct"], "quantiles": m["quantiles"],
                "bounds": [float(v) for v in rf.bounds],
            },
        }
    else:
        gdf, _ = resolver.frame(source)
        source_crs = gdf.crs
        target = pv.resolve_display_crs(gdf.crs, crs_req, gdf)
        if target is not None and gdf.crs is not None and gdf.crs != target:
            gdf = gdf.to_crs(target)
        svg = pv.vector_svg(gdf, title)
        png = pv.vector_png(gdf) if want_png else b""
        types: dict[str, int] = {}
        for t in map(str, gdf.geometry.geom_type):
            types[t] = types.get(t, 0) + 1
        fields = []
        for col in gdf.columns:
            if col == gdf.geometry.name:
                continue
            samples = [json_safe(v) for v in pd.unique(gdf[col].dropna())[:2]]
            fields.append({"name": str(col), "dtype": str(gdf[col].dtype), "samples": samples})
        out = {
            "kind": "vector",
            "meta": {
                "count": int(len(gdf)),
                "geometry_types": types,
                "bounds": [float(v) for v in gdf.total_bounds] if len(gdf) else None,
                "fields": fields[:40],
            },
        }
    display = target if target is not None else source_crs
    out["meta"].update({
        "crs": display.to_string() if display is not None else None,
        "crs_label": pv.crs_label(display),
        "crs_units": ("度" if display is not None and display.is_geographic else
                      (display.axis_info[0].unit_name if display is not None else "")),
        "source_crs": source_crs.to_string() if source_crs is not None else None,
    })
    out.update({
        "source": source,
        "svg": svg,
        "png_base64": base64.b64encode(png).decode("ascii") if want_png and png else None,
        "warnings": list(dict.fromkeys(resolver.warnings)),
    })
    return out


def card(workdir: Path, action: str, payload: dict, result: dict, opts: dict) -> dict:
    """对话内结果卡片：随 inspect / analyze / visualize 的结果一起生成，省一次进程启动。

    opts：{crs?, png?}。返回 {kind, svg, png_base64?, chips...}；生成失败不影响主结果。
    """
    import base64

    if action == "visualize" and result.get("image_path"):
        from claude_gis.viz import preview as pv

        svg, png = pv.image_svg(str(result["image_path"]), str(payload.get("title") or ""))
        return {
            "kind": "map",
            "svg": svg,
            "png_base64": base64.b64encode(png).decode("ascii") if opts.get("png") else None,
            "meta": {},
            "warnings": [],
        }
    if action == "analyze" and result.get("artifact_id"):
        source = str(result["artifact_id"])
    elif action == "inspect" and payload.get("path"):
        source = str(payload["path"])
    else:
        return {}
    out = preview(workdir, {"source": source, "crs": opts.get("crs") or "source",
                            "png": bool(opts.get("png"))})
    out.pop("source", None)
    return out


def list_inventory(workdir: Path, payload: dict) -> dict:
    """数据集 + artifacts 清单（面板数据源）。

    数据集来源优先级：
    1. agent-used：artifact 元数据里记录的 inputs（Claude 实际用过的文件）；
    2. workspace：请求给定的 datasets_dirs（通常是会话工作目录）做有上限的浅扫描；
    3. 兼容旧 datasets_dir 单目录参数。
    坏 meta 跳过并计数，绝不让整个清单失败。
    """
    roots: list[str] = []
    for key in ("datasets_dirs", "datasets_dir"):
        v = payload.get(key)
        if isinstance(v, str) and v:
            roots.append(v)
        elif isinstance(v, list):
            roots.extend(str(x) for x in v if str(x))
    out: dict = {"datasets": [], "artifacts": [], "skipped_corrupt": 0}
    seen_paths: set[str] = set()

    def _push_dataset(path_str: str, origin: str) -> None:
        p = Path(path_str)
        key = str(p).replace("\\", "/").lower()
        if key in seen_paths or not p.is_file():
            return
        ext = p.suffix.lower()
        if ext == ".json" and not _looks_like_geojson(p):
            return  # package.json/tsconfig.json 等非空间 JSON 不进清单
        fmt = next((k for k, exts in
                    {"geojson": (".geojson", ".json"), "gpkg": (".gpkg",),
                     "shapefile": (".shp",), "csv": (".csv",)}.items()
                    if ext in exts), None)
        if fmt is None:
            return
        seen_paths.add(key)
        out["datasets"].append({
            "name": p.stem,
            "path": str(p),
            "format": fmt,
            "size_bytes": p.stat().st_size,
            "origin": origin,
        })

    store = ArtifactStore(Path(workdir))
    workdir_real = str(store.root_real)
    metas: list[dict] = []
    for artifact_id in store.list_ids():
        try:
            meta = store.get(artifact_id)
        except GisError:
            out["skipped_corrupt"] += 1
            continue
        metas.append(meta)
        # agent 用过的输入文件 → 数据集清单（agent 自主决定显示什么的第一来源）
        for inp in meta.get("inputs") or []:
            p = inp.get("path")
            if p and not str(p).replace("\\", "/").lower().startswith(
                    workdir_real.replace("\\", "/").lower()):
                _push_dataset(str(p), "agent-used")

    for root_str in roots:
        root = Path(root_str)
        if not root.is_absolute() or not root.is_dir():
            continue
        _scan_workspace(root, _push_dataset, limit=300)

    out["datasets"].sort(key=lambda d: ({"agent-used": 0, "workspace": 1}.get(d["origin"], 2), d["name"]))

    for meta in metas:
        res = meta.get("result") or {}
        out["artifacts"].append({
            "artifact_id": meta["artifact_id"],
            "kind": meta.get("kind"),
            "title": meta.get("title", ""),
            "created": meta.get("created", ""),
            "count": res.get("count", 0),
            "geometry_types": res.get("geometry_types", []),
            "summary": (meta.get("summary") or [])[:6],
            "warnings": len(meta.get("warnings") or []),
            "steps": meta.get("steps") or [],
            "intermediate_layers": meta.get("intermediate_layers") or {},
            "images": ([meta["image"]] if meta.get("image") else [])
            + [img.get("path") for img in meta.get("images") or [] if img.get("path")],
        })
    out["artifacts"].reverse()  # 新的在前
    return out


_SKIP_DIRS = {"node_modules", ".git", "__pycache__", ".venv", "venv", "dist",
              "build", ".pytest_cache", "artifacts", ".claude", ".claude-plugin"}


def _looks_like_geojson(path: Path) -> bool:
    """只读前 4KB 嗅探：含 FeatureCollection/Feature/features/geometries 才算空间 JSON。"""
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as fh:
            head = fh.read(4096).lower()
        return any(token in head for token in
                   ('"featurecollection"', '"features"', '"geometries"', '"type": "feature"'))
    except OSError:
        return False


def _scan_workspace(root: Path, push, *, limit: int, max_depth: int = 3) -> None:
    """有上限的浅扫描：深度 ≤3、跳过依赖/缓存目录、最多 limit 个文件。"""
    found = 0
    stack: list[tuple[Path, int]] = [(root, 0)]
    while stack and found < limit:
        cur, depth = stack.pop()
        try:
            entries = sorted(cur.iterdir())
        except OSError:
            continue
        for entry in entries:
            if found >= limit:
                break
            if entry.is_file():
                if entry.suffix.lower() in (".geojson", ".json", ".gpkg", ".shp", ".csv"):
                    push(str(entry), "workspace")
                    found += 1
            elif depth + 1 <= max_depth and entry.name not in _SKIP_DIRS \
                    and not entry.name.startswith("edge-profile"):
                stack.append((entry, depth + 1))
