from __future__ import annotations

from typing import Any

from .catalog import Catalog
from .layout_geom import intersects, pack_page

MAX_PAGE_WIDGETS = 40
CLASSIC_THEMES = (
    "instrument", "hud", "mecha", "stealth", "arcade", "drone", "scope",
    "heatsink", "loadout", "armor", "sweep",
)
# 单页模式：去掉翻页后只保留一页，把资源让给组件细节和局部动画。
MAX_TOTAL_WIDGETS = 20
MAX_PAGES = 1


class LayoutError(ValueError):
    pass


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_widgets(
    widgets: Any,
    profile: dict[str, Any],
    catalog: Catalog,
    seen_ids: set[str],
    page_label: str = "",
    alt_profile: dict[str, Any] | None = None,
) -> None:
    prefix = f"{page_label} " if page_label else ""
    if not isinstance(widgets, list):
        raise LayoutError(f"{prefix}widgets 必须是数组")
    if len(widgets) > MAX_PAGE_WIDGETS:
        raise LayoutError(f"{prefix}单页组件数量超过上限 {MAX_PAGE_WIDGETS}")

    cols = int(profile["cols"])
    rows = int(profile["rows"])
    placed: list[dict[str, Any]] = []
    alt_placed: list[dict[str, Any]] = []
    alt_cols = int(alt_profile["cols"]) if alt_profile else 0
    alt_rows = int(alt_profile["rows"]) if alt_profile else 0

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
            raise LayoutError(f"{prefix}组件 {widget_id} 的类型未知: {widget_type}")
        for key in ("x", "y", "w", "h"):
            if not _is_int(widget.get(key)):
                raise LayoutError(f"{prefix}组件 {widget_id} 的 {key} 必须是整数")
        x, y, w, h = (int(widget[k]) for k in ("x", "y", "w", "h"))
        if not catalog.is_valid_size(widget_type, w, h):
            allowed = "、".join(f"{ow}×{oh}" for ow, oh in catalog.size_options(widget_type))
            raise LayoutError(
                f"{prefix}组件 {widget_id} 尺寸 {w}×{h} 不合法，可选: {allowed}"
            )
        if x < 0 or y < 0 or x + w > cols or y + h > rows:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 超出画布范围 {cols}×{rows}"
            )

        options = widget.get("options", {})
        if not isinstance(options, dict):
            raise LayoutError(f"{prefix}组件 {widget_id} 的 options 必须是对象")

        bindings = widget.get("bindings")
        if not isinstance(bindings, dict):
            raise LayoutError(f"{prefix}组件 {widget_id} 的 bindings 必须是对象")

        binding_defs = {b["key"]: b for b in definition.get("bindings", [])}
        for key in bindings:
            if key not in binding_defs:
                raise LayoutError(f"{prefix}组件 {widget_id} 不支持绑定 {key}")

        for key, binding_def in binding_defs.items():
            metric_id = bindings.get(key)
            if binding_def.get("required") and not metric_id:
                raise LayoutError(f"{prefix}组件 {widget_id} 缺少必填绑定 {key}")
            if metric_id is None:
                continue
            if not isinstance(metric_id, str):
                raise LayoutError(f"{prefix}组件 {widget_id} 的绑定 {key} 必须是字符串")
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
                raise LayoutError(f"{prefix}组件 {widget_id} 的 rows 必须是数组")
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
            if intersects(rect, other):
                raise LayoutError(f"{prefix}组件 {widget_id} 与 {other['id']} 重叠")
        placed.append({"id": widget_id, **rect})

        alt = widget.get("alt")
        if alt is None:
            continue
        if alt_profile is None:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 设置了 alt 位置，但当前 profile 没有对应方向"
            )
        if not isinstance(alt, dict):
            raise LayoutError(f"{prefix}组件 {widget_id} 的 alt 必须是对象")
        for key in ("x", "y"):
            if not _is_int(alt.get(key)):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的 alt.{key} 必须是整数"
                )
        for key in ("w", "h"):
            if key in alt and not _is_int(alt.get(key)):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的 alt.{key} 必须是整数"
                )
        ax, ay = int(alt["x"]), int(alt["y"])
        aw = int(alt.get("w", w))
        ah = int(alt.get("h", h))
        if aw != w or ah != h:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 的 alt 尺寸必须与主方向一致（{w}×{h}）"
            )
        if ax < 0 or ay < 0 or ax + aw > alt_cols or ay + ah > alt_rows:
            raise LayoutError(
                f"{prefix}组件 {widget_id} 的 alt 超出画布范围 {alt_cols}×{alt_rows}"
            )
        alt_rect = {"id": widget_id, "x": ax, "y": ay, "w": aw, "h": ah}
        for other in alt_placed:
            if intersects(alt_rect, other):
                raise LayoutError(
                    f"{prefix}组件 {widget_id} 的 alt 与 {other['id']} 重叠"
                )
        alt_placed.append(alt_rect)

    if alt_profile is not None:
        if pack_page(list(widgets), profile, alt_profile, catalog) is None:
            raise LayoutError(
                f"{prefix}组件在 {alt_profile.get('label') or '另一方向'} 放不下，"
                "请减少组件或拆页"
            )


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
    alt_name = profile.get("alt")
    alt_profile = catalog.profiles.get(alt_name) if isinstance(alt_name, str) else None

    theme = layout.get("theme")
    if theme is not None:
        if not isinstance(theme, str):
            raise LayoutError(f"未知主题: {theme}")
        if theme not in catalog.themes and theme not in catalog.theme_aliases:
            raise LayoutError(f"未知主题: {theme}")

    framework = layout.get("framework")
    if framework is not None and framework not in ("widgets", "classic"):
        raise LayoutError(f"未知界面框架: {framework}（接受 widgets / classic）")
    classic_theme = layout.get("classic_theme")
    if classic_theme is not None and classic_theme not in CLASSIC_THEMES:
        raise LayoutError(f"未知传统主题: {classic_theme}（接受 {' / '.join(CLASSIC_THEMES)}）")

    if version == 1:
        _validate_widgets(
            layout.get("widgets"), profile, catalog, set(), alt_profile=alt_profile
        )
        return layout

    pages = layout.get("pages")
    if not isinstance(pages, list):
        raise LayoutError("pages 必须是数组")
    if not 1 <= len(pages) <= MAX_PAGES:
        raise LayoutError(f"页面数量必须在 1..{MAX_PAGES} 之间")

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
            alt_profile=alt_profile,
        )

    return layout
