from __future__ import annotations

import time
from collections import defaultdict, deque
from dataclasses import dataclass
from typing import Any

from .catalog import Catalog

HISTORY_MAX = 120


@dataclass
class MetricValue:
    value: Any = None
    available: bool = False
    ts: float = 0.0


class MetricStore:
    def __init__(self, catalog: Catalog) -> None:
        self.catalog = catalog
        self.values: dict[str, MetricValue] = {
            mid: MetricValue() for mid in catalog.metrics
        }
        self.history: dict[str, deque[float]] = defaultdict(
            lambda: deque(maxlen=HISTORY_MAX)
        )

    def update(self, metric_id: str, value: Any, available: bool = True) -> None:
        if metric_id not in self.catalog.metrics:
            raise KeyError(f"未知指标 {metric_id}")
        if value is None:
            available = False
        ts = time.time()
        self.values[metric_id] = MetricValue(value=value, available=available, ts=ts)
        metric = self.catalog.metrics[metric_id]
        if (
            available
            and metric.get("chart")
            and isinstance(value, (int, float))
            and not isinstance(value, bool)
        ):
            self.history[metric_id].append(float(value))

    def snapshot(self, ids: set[str] | None = None) -> dict[str, Any]:
        values: dict[str, dict[str, Any]] = {}
        for metric_id, mv in self.values.items():
            if ids is not None and metric_id not in ids:
                continue
            values[metric_id] = {
                "v": mv.value,
                "a": mv.available,
                "t": round(mv.ts, 3) if mv.ts else 0,
            }
        return {
            "type": "snapshot",
            "v": 1,
            "ts": round(time.time(), 3),
            "values": values,
        }
