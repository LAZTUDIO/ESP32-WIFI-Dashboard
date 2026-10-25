"""布局归一化。

- 旧主题 id 映射到新四套主题；
- 所有组件尺寸归位到该类型允许的尺寸；
- 保留合法的主方向位置，重叠时按原顺序重新摆放；
- 另一方向放不下时丢弃该页 alt（由投影自动重排），或迁移时重新拆分页面。
"""

from __future__ import annotations

import copy
import json
from typing import Any

from .catalog import Catalog
from .layout_geom import (
    clamp_rect,
    intersects,
    iter_widgets,
    pack_from_scratch,
    pack_page,
    find_spot,
    widget_size,
)

MAX_PAGES = 1
MAX_TOTAL_WIDGETS = 20


def _widgets(layout: dict[str, Any]) -> list[dict[str, Any]]:
    return list(iter_widgets(layout))


def _base_ok(widgets: list[dict[str, Any]], profile: dict[str, Any], catalog: Catalog) -> bool:
    cols, rows = int(profile["cols"]), int(profile["rows"])
    placed: list[dict[str, int]] = []
    for widget in widgets:
        w, h = widget_size(widget, catalog)
        try:
            rect = {k: int(widget[k]) for k in ("x", "y")}
            rect.update({"w": w, "h": h})
        except (KeyError, TypeError, ValueError):
            return False
        if not catalog.is_valid_size(widget.get("type"), w, h):
            return False
        if widget.get("w") != w or widget.get("h") != h:
            return False
        if rect["x"] < 0 or rect["y"] < 0 or rect["x"] + w > cols or rect["y"] + h > rows:
            return False
        if any(intersects(rect, other) for other in placed):
            return False
        placed.append(rect)
    return True


def _place_in_order(
    widgets: list[dict[str, Any]], profile: dict[str, Any], catalog: Catalog
) -> dict[str, dict[str, int]] | None:
    occupied: list[dict[str, int]] = []
    placed: dict[str, dict[str, int]] = {}
    for widget in widgets:
        w, h = widget_size(widget, catalog)
        try:
            desired = {"x": int(widget["x"]), "y": int(widget["y"]), "w": w, "h": h}
        except (KeyError, TypeError, ValueError):
            desired = {"x": 0, "y": 0, "w": w, "h": h}
        desired = clamp_rect(desired, profile, w, h)
        spot = find_spot(desired, profile, occupied)
        if spot is None:
            spot = find_spot({"x": 0, "y": 0, "w": w, "h": h}, profile, occupied)
        if spot is None:
            return None
        occupied.append(spot)
        placed[str(widget.get("id"))] = spot
    return placed


def _normalize_widget_sizes(widgets: list[dict[str, Any]], catalog: Catalog) -> None:
    for widget in widgets:
        raw_w = widget.get("w")
        raw_h = widget.get("h")
        if not isinstance(raw_w, int) or not isinstance(raw_h, int):
            raw_w, raw_h = catalog.default_size(widget.get("type"))
        w, h = catalog.nearest_size(widget.get("type"), raw_w, raw_h)
        widget["w"] = w
        widget["h"] = h
        alt = widget.get("alt")
        if isinstance(alt, dict):
            if not isinstance(alt.get("x"), int) or not isinstance(alt.get("y"), int):
                widget.pop("alt", None)
            else:
                widget["alt"] = {"x": int(alt["x"]), "y": int(alt["y"])}


def _normalize_alt(
    widgets: list[dict[str, Any]],
    profile: dict[str, Any],
    alt_profile: dict[str, Any] | None,
    catalog: Catalog,
) -> None:
    if alt_profile is None:
        for widget in widgets:
            widget.pop("alt", None)
        return
    alts: list[dict[str, int]] = []
    for widget in widgets:
        alt = widget.get("alt")
        if not isinstance(alt, dict):
            continue
        w, h = widget_size(widget, catalog)
        rect = {"x": int(alt["x"]), "y": int(alt["y"]), "w": w, "h": h}
        cols, rows = int(alt_profile["cols"]), int(alt_profile["rows"])
        if (
            rect["x"] < 0
            or rect["y"] < 0
            or rect["x"] + w > cols
            or rect["y"] + h > rows
            or any(intersects(rect, other) for other in alts)
        ):
            for item in widgets:
                item.pop("alt", None)
            return
        alts.append(rect)
    if pack_page(widgets, profile, alt_profile, catalog) is None:
        for widget in widgets:
            widget.pop("alt", None)


def _as_v2(layout: dict[str, Any]) -> dict[str, Any]:
    if layout.get("v") == 2 and isinstance(layout.get("pages"), list):
        return layout
    widgets = layout.get("widgets") if isinstance(layout.get("widgets"), list) else []
    result = {
        "v": 2,
        "profile": layout.get("profile"),
        "theme": layout.get("theme"),
        "pages": [{"name": "主页", "widgets": widgets}],
    }
    # 界面框架字段跨版本透传
    for key in ("framework", "classic_theme"):
        if key in layout:
            result[key] = layout[key]
    return result


def normalize_layout(
    layout: dict[str, Any], catalog: Catalog
) -> dict[str, Any] | None:
    """返回单页归一化后的新布局；放不下时返回 None。"""
    result = _as_v2(copy.deepcopy(layout))
    result.pop("page_animation", None)
    result["v"] = 2
    result["theme"] = catalog.resolve_theme(result.get("theme"))
    profile_name = result.get("profile")
    profile = catalog.profiles.get(profile_name)
    if profile is None:
        return None
    alt_name = profile.get("alt")
    alt_profile = catalog.profiles.get(alt_name) if isinstance(alt_name, str) else None

    source_pages = result.get("pages") or []
    flat: list[dict[str, Any]] = []
    for page in source_pages:
        for widget in page.get("widgets", []) if isinstance(page, dict) else []:
            flat.append(widget)
    if not flat:
        return result
    _normalize_widget_sizes(flat, catalog)

    for page in result["pages"]:
        widgets = list(page.get("widgets", []))
        if _base_ok(widgets, profile, catalog):
            _normalize_alt(widgets, profile, alt_profile, catalog)
            continue
        placed = _place_in_order(widgets, profile, catalog)
        if placed is None:
            return None
        for widget in widgets:
            rect = placed[str(widget.get("id"))]
            widget["x"], widget["y"] = rect["x"], rect["y"]
        _normalize_alt(widgets, profile, alt_profile, catalog)
    return result


def dump_layout(layout: dict[str, Any]) -> str:
    return json.dumps(layout, ensure_ascii=False, indent=2) + "\n"


def compact_single_page(
    layout: dict[str, Any], catalog: Catalog
) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    """把多页布局压成一页：优先保留状态条、2 个 KPI、服务器卡和桌宠。"""
    result = _as_v2(copy.deepcopy(layout))
    result["theme"] = catalog.resolve_theme(result.get("theme"))
    profile = catalog.profiles.get(result.get("profile"))
    if profile is None:
        return None, []
    alt_name = profile.get("alt")
    alt_profile = catalog.profiles.get(alt_name) if isinstance(alt_name, str) else None
    flat = [
        widget
        for page in result.get("pages", [])
        for widget in page.get("widgets", [])
    ]
    if not flat:
        return None, []
    _normalize_widget_sizes(flat, catalog)

    counts = {"kpi": 0}
    def rank(item: tuple[int, dict[str, Any]]) -> tuple[int, int]:
        index, widget = item
        widget_type = widget.get("type")
        if widget_type == "status_pill":
            return (0, index)
        if widget_type == "kpi":
            counts["kpi"] += 1
            return (1 if counts["kpi"] <= 2 else 4, index)
        if widget_type == "server_card":
            return (2, index)
        if widget_type == "desktop_pet":
            return (3, index)
        if widget_type == "sparkline":
            return (5, index)
        return (6, index)

    ordered = [widget for _, widget in sorted(enumerate(flat), key=rank)]
    selected: list[dict[str, Any]] = []
    dropped: list[dict[str, Any]] = []
    for widget in ordered:
        candidate = selected + [widget]
        base_ok = pack_from_scratch(candidate, profile, catalog) is not None
        alt_ok = (
            alt_profile is None
            or pack_from_scratch(candidate, alt_profile, catalog) is not None
        )
        if base_ok and alt_ok:
            selected.append(widget)
        else:
            dropped.append(widget)

    if not selected:
        return None, dropped
    base_placed = pack_from_scratch(selected, profile, catalog)
    alt_placed = (
        pack_from_scratch(selected, alt_profile, catalog)
        if alt_profile is not None
        else None
    )
    if base_placed is None or (alt_profile is not None and alt_placed is None):
        return None, dropped
    for widget in selected:
        rect = base_placed[str(widget.get("id"))]
        widget["x"], widget["y"] = rect["x"], rect["y"]
        widget["w"], widget["h"] = rect["w"], rect["h"]
        if alt_placed is not None:
            alt_rect = alt_placed[str(widget.get("id"))]
            widget["alt"] = {"x": alt_rect["x"], "y": alt_rect["y"]}
        else:
            widget.pop("alt", None)
    result["pages"] = [{"name": "主页", "widgets": selected}]
    return result, dropped


def is_normalized(layout: dict[str, Any], catalog: Catalog) -> bool:
    """已经是新主题 + 固定尺寸 + 两方向可排时不再改写，保证迁移幂等。"""
    if not isinstance(layout, dict) or layout.get("v") != 2:
        return False
    if layout.get("theme") not in catalog.themes:
        return False
    profile = catalog.profiles.get(layout.get("profile"))
    if profile is None:
        return False
    alt_name = profile.get("alt")
    alt_profile = catalog.profiles.get(alt_name) if isinstance(alt_name, str) else None
    pages = layout.get("pages")
    if not isinstance(pages, list) or not 1 <= len(pages) <= MAX_PAGES:
        return False
    total = 0
    for page in pages:
        if not isinstance(page, dict) or not isinstance(page.get("widgets"), list):
            return False
        widgets = list(page["widgets"])
        total += len(widgets)
        if total > MAX_TOTAL_WIDGETS:
            return False
        if not _base_ok(widgets, profile, catalog):
            return False
        alt_rects: list[dict[str, int]] = []
        for widget in widgets:
            alt = widget.get("alt")
            if alt is None:
                continue
            if not isinstance(alt, dict) or any(k in alt for k in ("w", "h")):
                return False
            if not isinstance(alt.get("x"), int) or not isinstance(alt.get("y"), int):
                return False
            w, h = widget_size(widget, catalog)
            rect = {"x": int(alt["x"]), "y": int(alt["y"]), "w": w, "h": h}
            if alt_profile is None:
                return False
            cols, rows = int(alt_profile["cols"]), int(alt_profile["rows"])
            if (
                rect["x"] < 0
                or rect["y"] < 0
                or rect["x"] + w > cols
                or rect["y"] + h > rows
                or any(intersects(rect, other) for other in alt_rects)
            ):
                return False
            alt_rects.append(rect)
        if alt_profile is not None and pack_page(widgets, profile, alt_profile, catalog) is None:
            return False
    return True
