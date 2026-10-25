from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any

from ..config import SHARED_DIR


@dataclass
class Catalog:
    metrics: dict[str, dict[str, Any]]
    metric_list: list[dict[str, Any]]
    widgets: dict[str, dict[str, Any]]
    widget_list: list[dict[str, Any]]
    profiles: dict[str, dict[str, Any]]
    themes: dict[str, dict[str, Any]]
    theme_list: list[dict[str, Any]]
    theme_default: str = "neon-cyber"
    theme_aliases: dict[str, str] = field(default_factory=dict)
    version: int = 1

    def resolve_theme(self, theme_id: Any) -> str:
        """旧主题 id 通过 aliases 映射到新四套主题之一。"""
        value = str(theme_id or "")
        if value in self.themes:
            return value
        alias = self.theme_aliases.get(value)
        if alias in self.themes:
            return alias
        return self.theme_default

    def size_options(self, widget_type: Any) -> list[tuple[int, int]]:
        definition = self.widgets.get(str(widget_type or "")) or {}
        raw = definition.get("size_options") or [definition.get("default_size") or [1, 1]]
        options: list[tuple[int, int]] = []
        for size in raw:
            if isinstance(size, list) and len(size) == 2:
                w, h = int(size[0]), int(size[1])
                if w > 0 and h > 0:
                    options.append((w, h))
        return options or [(1, 1)]

    def default_size(self, widget_type: Any) -> tuple[int, int]:
        definition = self.widgets.get(str(widget_type or "")) or {}
        size = definition.get("default_size")
        if isinstance(size, list) and len(size) == 2:
            return int(size[0]), int(size[1])
        return self.size_options(widget_type)[0]

    def nearest_size(self, widget_type: Any, w: int, h: int) -> tuple[int, int]:
        """把任意 w/h 归位到该类型允许的尺寸；距离相同优先更小面积。"""
        options = self.size_options(widget_type)
        if (w, h) in options:
            return w, h
        return min(
            options,
            key=lambda size: (
                abs(size[0] - w) + abs(size[1] - h),
                size[0] * size[1],
            ),
        )

    def is_valid_size(self, widget_type: Any, w: int, h: int) -> bool:
        return (int(w), int(h)) in self.size_options(widget_type)


def _load(name: str) -> dict[str, Any]:
    path = SHARED_DIR / "catalog" / name
    return json.loads(path.read_text(encoding="utf-8"))


def load_catalog() -> Catalog:
    metrics_doc = _load("metrics.json")
    widgets_doc = _load("widgets.json")
    profiles_doc = _load("profiles.json")
    themes_doc = _load("themes.json")

    metric_list = metrics_doc.get("metrics", [])
    widget_list = widgets_doc.get("widgets", [])
    theme_list = themes_doc.get("themes", [])

    metrics = {m["id"]: m for m in metric_list}
    if len(metrics) != len(metric_list):
        raise ValueError("metrics.json 存在重复 id")

    widgets = {w["type"]: w for w in widget_list}
    if len(widgets) != len(widget_list):
        raise ValueError("widgets.json 存在重复 type")
    for widget in widget_list:
        options = widget.get("size_options")
        if not isinstance(options, list) or not options:
            raise ValueError(f"widgets.json 组件 {widget['type']} 缺少 size_options")
        for size in options:
            if not isinstance(size, list) or len(size) != 2 or not all(
                isinstance(v, int) and v > 0 for v in size
            ):
                raise ValueError(
                    f"widgets.json 组件 {widget['type']} 的 size_options 非法: {size}"
                )
        default = widget.get("default_size")
        if default not in options:
            raise ValueError(
                f"widgets.json 组件 {widget['type']} 的 default_size 不在 size_options 里"
            )

    themes = {t["id"]: t for t in theme_list}
    if len(themes) != len(theme_list):
        raise ValueError("themes.json 存在重复 id")
    for theme in theme_list:
        tokens = theme.get("tokens")
        if not isinstance(tokens, dict) or not tokens.get("bg"):
            raise ValueError(f"themes.json 主题 {theme['id']} 缺少 tokens")
        if not isinstance(tokens.get("skin"), str) or not tokens["skin"]:
            raise ValueError(f"themes.json 主题 {theme['id']} 缺少 skin")

    theme_default = str(themes_doc.get("default") or "neon-cyber")
    if theme_default not in themes:
        theme_default = next(iter(themes))
    raw_aliases = themes_doc.get("aliases")
    theme_aliases = {
        str(key): str(value)
        for key, value in (raw_aliases.items() if isinstance(raw_aliases, dict) else [])
        if str(value) in themes
    }

    return Catalog(
        metrics=metrics,
        metric_list=metric_list,
        widgets=widgets,
        widget_list=widget_list,
        profiles=profiles_doc.get("profiles", {}),
        themes=themes,
        theme_list=theme_list,
        theme_default=theme_default,
        theme_aliases=theme_aliases,
        version=int(metrics_doc.get("version", 1)),
    )
