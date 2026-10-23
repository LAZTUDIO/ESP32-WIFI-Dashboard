from __future__ import annotations

import json
from dataclasses import dataclass
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
    version: int = 1


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

    themes = {t["id"]: t for t in theme_list}
    if len(themes) != len(theme_list):
        raise ValueError("themes.json 存在重复 id")
    for theme in theme_list:
        tokens = theme.get("tokens")
        if not isinstance(tokens, dict) or not tokens.get("bg"):
            raise ValueError(f"themes.json 主题 {theme['id']} 缺少 tokens")

    return Catalog(
        metrics=metrics,
        metric_list=metric_list,
        widgets=widgets,
        widget_list=widget_list,
        profiles=profiles_doc.get("profiles", {}),
        themes=themes,
        theme_list=theme_list,
        version=int(metrics_doc.get("version", 1)),
    )
