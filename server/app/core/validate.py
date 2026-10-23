from __future__ import annotations

from typing import Any

from .catalog import Catalog

MAX_PAGE_WIDGETS = 40
# 实测 320x480 板：25 个混合组件冷启动后内部最大连续块仅 3.4KB，
# WebSocket 断线重建会失败；20 个以内可保留约 7KB 余量。
MAX_TOTAL_WIDGETS = 20
MAX_PAGES = 4
PAGE_ANIMATIONS = {"slide", "fade", "none", "stack", "zoom", "flip", "rotate"}


class LayoutError(ValueError):
    pass


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _intersects(a: dict[str, Any], b: dict[str, Any]) -> bool:
    return (
        a["x"] < b["x"] + b["w"]
        and b["x"] < a["x"] + a["w"]
        and a["y"] < b["y"] + b["h"]
        and b["y"] < a["y"] + a["h"]
    )


def _validate_widgets(
    widgets: Any,
    profile: dict[str, Any],
    catalog: Catalog,
    seen_ids: set[str],
    page_label: str = "",
) -> None:
    prefix = f"{page_label} " if page_label else ""
    if not isinstance(widgets, list):
        raise LayoutError(f"{prefix}widgets 必须是数组")
    if len(widgets) > MAX_PAGE_WIDGETS:
        raise LayoutError(
            f"{prefix}单页组件数量超过上限 {MAX_PAGE_WIDGETS}"
        )

    cols = int(profile["cols"])
    rows = int(profile["rows"])
    placed: list[dict[str, Any]] = []

    for index, widget in enumerate(widgets):
        if not isinstance(widget, dict):
            raise LayoutError(f"{prefix}第 {index + 1} 个组件不是对象")

        widget_id = widget.get("id")
        if not isinstance(widget_id, str) or not widget_id:
            raise LayoutError(f"{prefix}第 {index + 1} 个组件缺少 id")
        if widget_id in seen_ids:
            raise LayoutError(f"{prefix}组件 id 重复: {widget_id}")
        seen_ids.add(widget_id)

        widget_type = widget.get("type")
        definition = catalog.widgets.get(widget_type)
        if definition is None:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 的类型未知: {widget_type}"
            )

        for key in ("x", "y", "w", "h"):
            if not _is_int(widget.get(key)):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的 {key} 必须是整数"
                )
        x, y, w, h = (int(widget[k]) for k in ("x", "y", "w", "h"))

        options = widget.get("options", {})
        if not isinstance(options, dict):
            raise LayoutError(f"{prefix}组件 {widget_id} 的 options 必须是对象")

        min_size = definition.get("min_size", [1, 1])
        min_w, min_h = int(min_size[0]), int(min_size[1])
        if options.get("show_title") is False:
            min_h = max(1, min_h - 1)
        if w < min_w or h < min_h:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 尺寸过小，至少 {min_w}×{min_h} 网格"
            )
        if x < 0 or y < 0 or x + w > cols or y + h > rows:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 超出画布范围 {cols}×{rows}"
            )

        bindings = widget.get("bindings")
        if not isinstance(bindings, dict):
            raise LayoutError(
                f"{prefix}组件 {widget_id} 的 bindings 必须是对象"
            )

        binding_defs = {b["key"]: b for b in definition.get("bindings", [])}
        for key in bindings:
            if key not in binding_defs:
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 不支持绑定 {key}"
                )

        for key, binding_def in binding_defs.items():
            metric_id = bindings.get(key)
            if binding_def.get("required") and not metric_id:
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 缺少必填绑定 {key}"
                )
            if metric_id is None:
                continue
            if not isinstance(metric_id, str):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的绑定 {key} 必须是字符串"
                )
            metric = catalog.metrics.get(metric_id)
            if metric is None:
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 绑定了未知指标 {metric_id}"
                )
            if metric["type"] not in binding_def.get("types", []):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的绑定 {key} 类型不匹配: "
                    f"{metric_id} 是 {metric['type']}"
                )

        rows_spec = definition.get("rows")
        if rows_spec:
            row_items = options.get("rows")
            if not isinstance(row_items, list):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的 rows 必须是数组"
                )
            min_rows = int(rows_spec.get("min", 1))
            max_rows = int(rows_spec.get("max", 8))
            if not min_rows <= len(row_items) <= max_rows:
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的行数必须在 "
                    f"{min_rows}..{max_rows} 之间"
                )
            for row_index, row in enumerate(row_items):
                if not isinstance(row, dict):
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行不是对象"
                    )
                binding = row.get("binding")
                if not isinstance(binding, str) or not binding:
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行缺少 binding"
                    )
                row_metric = catalog.metrics.get(binding)
                if row_metric is None:
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行"
                        f"绑定了未知指标 {binding}"
                    )
                kind = row.get("kind", "auto")
                if kind not in ("auto", "status", "text", "number"):
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行"
                        f" kind 非法: {kind}"
                    )
                if kind == "number" and row_metric["type"] != "number":
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行"
                        f"需要数值指标，{binding} 是 {row_metric['type']}"
                    )
                label = row.get("label", "")
                if not isinstance(label, str):
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行"
                        f" label 必须是字符串"
                    )
                fmt = row.get("format", "")
                if not isinstance(fmt, str):
                    raise LayoutError(
                        f"{prefix}组件 {widget_id} 第 {row_index + 1} 行"
                        f" format 必须是字符串"
                    )

        rect = {"x": x, "y": y, "w": w, "h": h}
        for other in placed:
            if _intersects(rect, other):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 与 {other['id']} 重叠"
                )
        placed.append({"id": widget_id, **rect})


def validate_layout(layout: Any, catalog: Catalog) -> dict[str, Any]:
    if not isinstance(layout, dict):
        raise LayoutError("布局必须是 JSON 对象")

    version = layout.get("v")
    if version not in (1, 2):
        raise LayoutError("不支持的布局版本，当前接受 v=1 / v=2")

    profile_name = layout.get("profile")
    profile = catalog.profiles.get(profile_name)
    if profile is None:
        raise LayoutError(f"未知画布 profile: {profile_name}")

    theme = layout.get("theme")
    if theme is not None:
        if not isinstance(theme, str) or theme not in catalog.themes:
            raise LayoutError(f"未知主题: {theme}")

    if version == 1:
        _validate_widgets(
            layout.get("widgets"), profile, catalog, set()
        )
        return layout

    pages = layout.get("pages")
    if not isinstance(pages, list):
        raise LayoutError("pages 必须是数组")
    if not 1 <= len(pages) <= MAX_PAGES:
        raise LayoutError(f"页面数量必须在 1..{MAX_PAGES} 之间")

    animation = layout.get("page_animation", "slide")
    if animation not in PAGE_ANIMATIONS:
        raise LayoutError(f"翻页动画非法: {animation}")

    seen_ids: set[str] = set()
    total_widgets = 0
    for page_index, page in enumerate(pages):
        if not isinstance(page, dict):
            raise LayoutError(f"第 {page_index + 1} 页不是对象")
        name = page.get("name")
        if not isinstance(name, str) or not name.strip():
            raise LayoutError(f"第 {page_index + 1} 页缺少名称")
        page_widgets = page.get("widgets")
        if not isinstance(page_widgets, list):
            raise LayoutError(f"第 {page_index + 1} 页的 widgets 必须是数组")
        total_widgets += len(page_widgets)
        if total_widgets > MAX_TOTAL_WIDGETS:
            raise LayoutError(f"总组件数量超过上限 {MAX_TOTAL_WIDGETS}")
        _validate_widgets(
            page_widgets,
            profile,
            catalog,
            seen_ids,
            f"第 {page_index + 1} 页",
        )

    return layout
