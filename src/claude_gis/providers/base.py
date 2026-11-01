"""Providers: swappable execution backends behind one interface.

Governance rule (docs/architecture.md): providers implement existing primitive
classes; they never extend what the agent can express.
"""

from __future__ import annotations

from abc import ABC, abstractmethod

import geopandas as gpd


class Provider(ABC):
    """Pure computation contract for the four Phase-1 primitive classes.

    Implementations receive prepared GeoDataFrames (valid geometries, metric CRS
    where distances/areas matter) and must stay free of I/O and summary text.
    """

    # --- Query & Measure ---
    @abstractmethod
    def filter_attributes(self, gdf: gpd.GeoDataFrame, where: str) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def select_spatial(self, gdf: gpd.GeoDataFrame, predicate: str, ref: gpd.GeoDataFrame) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def measure(self, gdf: gpd.GeoDataFrame, measures: list[str]) -> tuple[gpd.GeoDataFrame, dict]: ...

    # --- Proximity ---
    @abstractmethod
    def buffer(self, gdf: gpd.GeoDataFrame, distance_m: float, dissolve: bool, quad_segs: int) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def within_distance(self, gdf: gpd.GeoDataFrame, ref: gpd.GeoDataFrame, distance_m: float) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def nearest(self, gdf: gpd.GeoDataFrame, ref: gpd.GeoDataFrame, k: int, join_attrs: list[str]) -> gpd.GeoDataFrame: ...

    # --- Overlay ---
    @abstractmethod
    def intersection(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def difference(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def union(self, a: gpd.GeoDataFrame, b: gpd.GeoDataFrame) -> gpd.GeoDataFrame: ...

    @abstractmethod
    def clip(self, a: gpd.GeoDataFrame, mask: gpd.GeoDataFrame) -> gpd.GeoDataFrame: ...

    # --- Zonal ---
    @abstractmethod
    def zonal_summarize(
        self,
        regions: gpd.GeoDataFrame,
        data: gpd.GeoDataFrame,
        stats: list[str],
        field: str | None,
    ) -> tuple[gpd.GeoDataFrame, dict]: ...
