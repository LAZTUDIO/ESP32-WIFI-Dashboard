from __future__ import annotations

import logging
from typing import Any

from .catalog import Catalog
from .layout_geom import iter_widgets, pack_from_scratch, pack_page

log = logging.getLogger("esp32wifi.resolve")


def canvas_info(profile: dict[str, Any], profile_name: str) -> dict[str, Any]:
    """固件不需要内置网格参数，resolved 布局里直接带上画布尺寸。"""
    return {
        "profile": profile_name,
        "width": int(profile["width"]),
        "height": int(profile["height"]),
        "cols": int(profile["cols"]),
        "rows": int(profile["rows"]),
        "gap": int(profile.get("gap", 0)),
        "padding": int(profile.get("padding", 0)),
    }


def _apply_rect(widget: dict[str, Any], rect: dict[str, int]) -> None:
    widget["x"] = int(rect["x"])
    widget["y"] = int(rect["y"])
    widget["w"] = int(rect["w"])
    widget["h"] = int(rect["h"])


def _pages(layout: dict[str, Any]) -> list[dict[str, Any]]:
    if layout.get("v") == 1:
        return [{"name": "第 1 页", "widgets": layout.get("widgets", [])}]
    return list(layout.get("pages", []))


def project_layout(
    layout: dict[str, Any], profile_name: str, catalog: Catalog
) -> dict[str, Any]:
    """把同一份固定尺寸组件布局投影到指定方向。

    组件的类型/绑定/选项/尺寸始终只有一份；位置以 layout.profile 为主方向，
    每个组件可选 alt 覆盖另一方向。没有 alt 的组件按比例映射位置后 first-fit。
    """
    target = catalog.profiles.get(profile_name)
    if target is None:
        return layout
    base_name = layout.get("profile")
    base = catalog.profiles.get(base_name)
    if base is not None and profile_name != base_name:
        for page in _pages(layout):
            widgets = list(page.get("widgets", []))
            if not widgets:
                continue
            placed = pack_page(widgets, base, target, catalog)
            if placed is None:
                placed = pack_from_scratch(widgets, target, catalog)
                if placed is None:
                    log.warning(
                        "layout page cannot fit %s -> %s, keep base positions",
                        base_name,
                        profile_name,
                    )
                    continue
            for widget in widgets:
                rect = placed.get(str(widget.get("id")))
                if rect is not None:
                    _apply_rect(widget, rect)
                widget.pop("alt", None)
        layout["profile"] = profile_name

    layout["canvas"] = canvas_info(target, profile_name)
    return layout


def resolve_layout(layout: dict[str, Any], catalog: Catalog) -> dict[str, Any]:
    """把指标标签/单位/精度写进布局，供 MCU 直接渲染，避免固件再拉目录。"""
    # 传统框架：固定界面主题，无小组件网格；注入默认值供固件直接分支
    if layout.get("framework") == "classic":
        if not layout.get("classic_theme"):
            layout["classic_theme"] = "instrument"
    layout.setdefault("framework", "widgets")

    theme_id = catalog.resolve_theme(layout.get("theme"))
    theme = catalog.themes.get(theme_id) or catalog.themes.get(catalog.theme_default)
    if theme:
        layout["theme"] = theme["id"]
        layout["theme_tokens"] = dict(theme.get("tokens") or {})
        layout["theme_skin"] = layout["theme_tokens"].get("skin", theme["id"])

    for widget in iter_widgets(layout):
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
