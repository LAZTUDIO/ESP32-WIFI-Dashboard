"""布局几何与尺寸排布工具。

组件尺寸从 widgets.json 的 size_options 里选，横竖屏尺寸一致；这里只处理位置：
- 从主方向映射到另一方向时，按像素比例换算 x/y，尺寸不变；
- 每个方向都用 first-fit 摆放矩形，避免重叠；
- 供校验、投影和迁移复用。
"""

from __future__ import annotations

from typing import Any, Iterable

from .catalog import Catalog


def iter_widgets(layout: dict[str, Any]) -> Iterable[dict[str, Any]]:
    if layout.get("v") == 1:
        yield from layout.get("widgets", [])
        return
    for page in layout.get("pages", []):
        yield from page.get("widgets", [])


def cell_size(profile: dict[str, Any]) -> tuple[float, float, float, float]:
    gap = float(profile.get("gap", 0))
    pad = float(profile.get("padding", 0))
    cols = int(profile["cols"])
    rows = int(profile["rows"])
    cw = (float(profile["width"]) - 2 * pad - (cols - 1) * gap) / cols
    ch = (float(profile["height"]) - 2 * pad - (rows - 1) * gap) / rows
    return cw, ch, gap, pad


def widget_size(widget: dict[str, Any], catalog: Catalog) -> tuple[int, int]:
    """组件自身 w/h 归位到该类型允许的尺寸；缺失时用默认尺寸。"""
    w = widget.get("w")
    h = widget.get("h")
    if isinstance(w, int) and isinstance(h, int) and w > 0 and h > 0:
        return catalog.nearest_size(widget.get("type"), w, h)
    return catalog.default_size(widget.get("type"))


def intersects(a: dict[str, int], b: dict[str, int]) -> bool:
    return (
        a["x"] < b["x"] + b["w"]
        and b["x"] < a["x"] + a["w"]
        and a["y"] < b["y"] + b["h"]
        and b["y"] < a["y"] + a["h"]
    )


def clamp_rect(
    rect: dict[str, Any], profile: dict[str, Any], w: int, h: int
) -> dict[str, int]:
    cols, rows = int(profile["cols"]), int(profile["rows"])
    w = min(max(1, w), cols)
    h = min(max(1, h), rows)
    x = max(0, min(int(rect.get("x", 0)), cols - w))
    y = max(0, min(int(rect.get("y", 0)), rows - h))
    return {"x": x, "y": y, "w": w, "h": h}


def map_position(
    rect: dict[str, Any],
    base: dict[str, Any],
    target: dict[str, Any],
    w: int,
    h: int,
) -> dict[str, int]:
    """尺寸保持不变，只按像素比例把左上角映射到目标网格。"""
    bcw, bch, bgap, bpad = cell_size(base)
    tcw, tch, tgap, tpad = cell_size(target)
    left = bpad + int(rect["x"]) * (bcw + bgap)
    top = bpad + int(rect["y"]) * (bch + bgap)
    cols, rows = int(target["cols"]), int(target["rows"])
    w = min(w, cols)
    h = min(h, rows)
    x = round(
        (left / float(base["width"]) * float(target["width"]) - tpad) / (tcw + tgap)
    )
    y = round(
        (top / float(base["height"]) * float(target["height"]) - tpad) / (tch + tgap)
    )
    return {
        "x": max(0, min(x, cols - w)),
        "y": max(0, min(y, rows - h)),
        "w": w,
        "h": h,
    }


def find_spot(
    rect: dict[str, int], profile: dict[str, Any], occupied: list[dict[str, int]]
) -> dict[str, int] | None:
    """同尺寸 first-fit，选离期望位置最近且不与已占用矩形重叠的位置。"""
    if not any(intersects(rect, other) for other in occupied):
        return rect
    cols, rows = int(profile["cols"]), int(profile["rows"])
    w, h = rect["w"], rect["h"]
    best: dict[str, int] | None = None
    best_score = 0
    for y in range(0, rows - h + 1):
        for x in range(0, cols - w + 1):
            cand = {"x": x, "y": y, "w": w, "h": h}
            if any(intersects(cand, other) for other in occupied):
                continue
            score = abs(x - rect["x"]) + abs(y - rect["y"])
            if best is None or score < best_score:
                best, best_score = cand, score
    return best


def pack_page(
    widgets: list[dict[str, Any]],
    base: dict[str, Any],
    target: dict[str, Any],
    catalog: Catalog,
) -> dict[str, dict[str, int]] | None:
    """把一页组件排到 target 方向；返回 id -> rect，放不下返回 None。

    显式写了 alt 的组件优先占位，其余组件按面积从大到小做 first-fit。
    """
    explicit: dict[str, dict[str, int]] = {}
    occupied: list[dict[str, int]] = []
    for widget in widgets:
        alt = widget.get("alt")
        if not isinstance(alt, dict):
            continue
        if not isinstance(alt.get("x"), int) or not isinstance(alt.get("y"), int):
            continue
        w, h = widget_size(widget, catalog)
        rect = clamp_rect(alt, target, w, h)
        explicit[str(widget.get("id"))] = rect
        occupied.append(rect)

    pending: list[tuple[dict[str, Any], dict[str, int]]] = []
    for widget in widgets:
        if str(widget.get("id")) in explicit:
            continue
        w, h = widget_size(widget, catalog)
        rect = map_position(widget, base, target, w, h)
        pending.append((widget, rect))
    pending.sort(key=lambda item: item[1]["w"] * item[1]["h"], reverse=True)

    placed = dict(explicit)
    for widget, rect in pending:
        spot = find_spot(rect, target, occupied)
        if spot is None:
            return None
        occupied.append(spot)
        placed[str(widget.get("id"))] = spot
    return placed


def pack_from_scratch(
    widgets: list[dict[str, Any]],
    target: dict[str, Any],
    catalog: Catalog,
) -> dict[str, dict[str, int]] | None:
    """忽略期望位置，按面积从大到小从左上角重新排；用于迁移拆页。"""
    cols, rows = int(target["cols"]), int(target["rows"])
    pending: list[tuple[str, int, int]] = []
    for widget in widgets:
        w, h = widget_size(widget, catalog)
        if w > cols or h > rows:
            return None
        pending.append((str(widget.get("id")), w, h))
    pending.sort(key=lambda item: item[1] * item[2], reverse=True)

    occupied: list[dict[str, int]] = []
    placed: dict[str, dict[str, int]] = {}
    for widget_id, w, h in pending:
        spot = None
        for y in range(0, rows - h + 1):
            for x in range(0, cols - w + 1):
                cand = {"x": x, "y": y, "w": w, "h": h}
                if not any(intersects(cand, other) for other in occupied):
                    spot = cand
                    break
            if spot is not None:
                break
        if spot is None:
            return None
        occupied.append(spot)
        placed[widget_id] = spot
    return placed
