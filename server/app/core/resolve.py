from __future__ import annotations

from typing import Any, Iterable

from .catalog import Catalog


def _iter_widgets(layout: dict[str, Any]) -> Iterable[dict[str, Any]]:
    if layout.get("v") == 1:
        yield from layout.get("widgets", [])
        return
    for page in layout.get("pages", []):
        yield from page.get("widgets", [])


def resolve_layout(layout: dict[str, Any], catalog: Catalog) -> dict[str, Any]:
    """把指标标签/单位/精度写进布局，供 MCU 直接渲染，避免固件再拉目录。"""
    for widget in _iter_widgets(layout):
        options = widget.setdefault("options", {})
        if not isinstance(options, dict):
            continue
        widget_type = widget.get("type")
        bindings = widget.get("bindings")
        if not isinstance(bindings, dict):
            bindings = {}

        labels = options.get("labels")
        if not isinstance(labels, dict):
            labels = {}
        units = options.get("units")
        if not isinstance(units, dict):
            units = {}
        precisions = options.get("precisions")
        if not isinstance(precisions, dict):
            precisions = {}
        types = options.get("types")
        if not isinstance(types, dict):
            types = {}

        for key, metric_id in bindings.items():
            metric = catalog.metrics.get(metric_id) if isinstance(metric_id, str) else None
            if metric is None:
                continue
            labels.setdefault(key, metric["label"])
            units[key] = metric.get("unit", "")
            precisions[key] = metric.get("precision", 0)
            types[key] = metric["type"]

        if labels:
            options["labels"] = labels
        if units:
            options["units"] = units
        if precisions:
            options["precisions"] = precisions
        if types:
            options["types"] = types

        if widget_type in ("kpi", "sparkline"):
            metric = catalog.metrics.get(bindings.get("value"))
            if metric:
                options.setdefault("metric_label", metric["label"])
                options["unit"] = metric.get("unit", "")
                options["precision"] = metric.get("precision", 0)
                options["metric_type"] = metric["type"]

        if widget_type == "metric_list":
            rows = options.get("rows")
            if isinstance(rows, list):
                for row in rows:
                    if not isinstance(row, dict):
                        continue
                    metric = catalog.metrics.get(row.get("binding"))
                    if metric is None:
                        continue
                    row["unit"] = metric.get("unit", "")
                    row["precision"] = metric.get("precision", 0)
                    row["metric_type"] = metric["type"]
                    if not row.get("label"):
                        row["label"] = metric["label"]

    return layout
