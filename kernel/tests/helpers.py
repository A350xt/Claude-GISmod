"""共用小工具（测试）。"""

from __future__ import annotations

from pathlib import Path

import geopandas as gpd


def write(gdf: gpd.GeoDataFrame, name: str, directory: Path | None = None) -> str:
    d = directory or Path(".")
    d.mkdir(parents=True, exist_ok=True)
    path = d / name
    gdf.to_file(path, driver="GeoJSON", index=False)
    return str(path)
