"""GeoCore — agent-native GIS runtime (Python kernel)."""

import os as _os
import sys as _sys


def _drop_foreign_proj_env() -> None:
    """系统级 PROJ_LIB/PROJ_DATA（如 PostgreSQL/PostGIS 安装写入的）会让 rasterio 的
    GDAL 读到版本不符的 proj.db（"contains DATABASE.LAYOUT.VERSION.MINOR"）。
    指向当前解释器之外的目录时丢弃，让各库使用自带的 PROJ 数据。"""
    prefix = _os.path.normcase(_os.path.realpath(_sys.prefix))
    for key in ("PROJ_LIB", "PROJ_DATA"):
        val = _os.environ.get(key)
        if val and not _os.path.normcase(_os.path.realpath(val)).startswith(prefix):
            del _os.environ[key]


_drop_foreign_proj_env()

__version__ = "0.2.0"
