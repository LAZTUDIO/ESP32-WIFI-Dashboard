from __future__ import annotations

import asyncio
import copy
import json
import sys
import unittest
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from app.adapters.panel import PanelAdapter  # noqa: E402
from app.core.catalog import load_catalog  # noqa: E402
from app.core.resolve import resolve_layout  # noqa: E402
from app.core.store import MetricStore  # noqa: E402
from app.core.validate import LayoutError, validate_layout  # noqa: E402

PRESETS = SERVER_DIR.parent / "shared" / "presets"


class ContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.catalog = load_catalog()
        self.store = MetricStore(self.catalog)

    def test_presets_validate(self) -> None:
        for profile in self.catalog.profiles:
            layout = json.loads(
                (PRESETS / f"default-{profile}.json").read_text(encoding="utf-8")
            )
            validate_layout(layout, self.catalog)

    def test_widget_default_bindings_exist(self) -> None:
        widgets = json.loads(
            (SERVER_DIR.parent / "shared" / "catalog" / "widgets.json").read_text(
                encoding="utf-8"
            )
        )["widgets"]
        for widget in widgets:
            for binding in widget.get("bindings", []):
                default = binding.get("default")
                if default:
                    self.assertIn(
                        default,
                        self.catalog.metrics,
                        f"{widget['type']}.{binding['key']} 默认测点缺失: {default}",
                    )
            for index, row in enumerate(
                (widget.get("rows") or {}).get("default", [])
            ):
                self.assertIn(
                    row.get("binding"),
                    self.catalog.metrics,
                    f"{widget['type']} 第 {index + 1} 行默认测点缺失",
                )

    def test_overlap_rejected(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        layout["widgets"][1]["x"] = 0
        layout["widgets"][1]["y"] = 0
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

    def test_binding_type_rejected(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        layout["widgets"][1]["bindings"]["value"] = "llm.model"
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

    def test_metric_list_rows(self) -> None:
        layout = {
            "v": 1,
            "profile": "320x480",
            "widgets": [
                {
                    "id": "list",
                    "type": "metric_list",
                    "x": 0,
                    "y": 0,
                    "w": 12,
                    "h": 4,
                    "bindings": {},
                    "options": {
                        "rows": [
                            {
                                "label": "服务",
                                "binding": "llm.online",
                                "kind": "status",
                            },
                            {
                                "label": "模型",
                                "binding": "llm.model",
                                "kind": "text",
                            },
                        ]
                    },
                },
                {
                    "id": "after-list",
                    "type": "kpi",
                    "x": 0,
                    "y": 6,
                    "w": 6,
                    "h": 3,
                    "bindings": {"value": "llm.decode_tps"},
                    "options": {"title": "解码速度"},
                },
            ],
        }
        validate_layout(layout, self.catalog)

        unknown = copy.deepcopy(layout)
        unknown["widgets"][0]["options"]["rows"][0]["binding"] = "nope"
        with self.assertRaises(LayoutError):
            validate_layout(unknown, self.catalog)

        wrong_kind = copy.deepcopy(layout)
        wrong_kind["widgets"][0]["options"]["rows"][0]["kind"] = "number"
        with self.assertRaises(LayoutError):
            validate_layout(wrong_kind, self.catalog)

    def test_page_layout_v2(self) -> None:
        layout = {
            "v": 2,
            "profile": "320x480",
            "page_animation": "fade",
            "pages": [
                {
                    "name": "概览",
                    "widgets": [
                        {
                            "id": "p1-status",
                            "type": "status_pill",
                            "x": 0,
                            "y": 0,
                            "w": 12,
                            "h": 2,
                            "bindings": {
                                "online": "llm.online",
                                "model": "llm.model",
                                "phase": "llm.phase",
                            },
                            "options": {},
                        },
                        {
                            "id": "p1-kpi",
                            "type": "kpi",
                            "x": 0,
                            "y": 2,
                            "w": 6,
                            "h": 3,
                            "bindings": {"value": "llm.decode_tps"},
                            "options": {"title": "解码"},
                        },
                    ],
                },
                {
                    "name": "服务",
                    "widgets": [
                        {
                            "id": "p2-kpi",
                            "type": "kpi",
                            "x": 0,
                            "y": 0,
                            "w": 6,
                            "h": 3,
                            "bindings": {"value": "tts.queue"},
                            "options": {"title": "TTS"},
                        }
                    ],
                },
            ],
        }
        validate_layout(layout, self.catalog)

        duplicate = copy.deepcopy(layout)
        duplicate["pages"][1]["widgets"][0]["id"] = "p1-kpi"
        with self.assertRaises(LayoutError):
            validate_layout(duplicate, self.catalog)

        too_many_pages = copy.deepcopy(layout)
        too_many_pages["pages"] = [
            copy.deepcopy(layout["pages"][0]) for _ in range(5)
        ]
        with self.assertRaises(LayoutError):
            validate_layout(too_many_pages, self.catalog)

        bad_animation = copy.deepcopy(layout)
        bad_animation["page_animation"] = "spin"
        with self.assertRaises(LayoutError):
            validate_layout(bad_animation, self.catalog)

    def test_theme_validation(self) -> None:
        self.assertIn("classic", self.catalog.themes)
        self.assertIn("strix", self.catalog.themes)
        self.assertIn("tuf", self.catalog.themes)

        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        layout["theme"] = "strix"
        validate_layout(layout, self.catalog)

        layout["theme"] = "nope"
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

    def test_resolve_injects_theme_tokens(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        layout["theme"] = "tuf"
        resolved = resolve_layout(layout, self.catalog)

        self.assertEqual(resolved["theme"], "tuf")
        tokens = resolved["theme_tokens"]
        self.assertEqual(tokens["bg"], "#0B0C0A")
        self.assertEqual(tokens["accent"], "#FFB000")
        self.assertEqual(tokens["radius"], 8)

    def test_resolve_layout_metadata(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        resolved = resolve_layout(layout, self.catalog)
        widgets = {w["id"]: w for w in resolved["widgets"]}

        kpi = widgets["w-dec"]["options"]
        self.assertEqual(kpi["unit"], "tok/s")
        self.assertEqual(kpi["metric_label"], "解码速度")

        server = widgets["w-server"]["options"]
        self.assertEqual(server["labels"]["cpu"], "CPU 使用率")
        self.assertEqual(server["units"]["cpu"], "%")
        self.assertEqual(server["labels"]["ip"], "主机 IP")

    def test_hidden_title_min_height(self) -> None:
        layout = {
            "v": 1,
            "profile": "320x480",
            "widgets": [
                {
                    "id": "compact",
                    "type": "kpi",
                    "x": 0,
                    "y": 0,
                    "w": 6,
                    "h": 2,
                    "bindings": {"value": "llm.decode_tps"},
                    "options": {"title": "解码", "show_title": False},
                }
            ],
        }
        validate_layout(layout, self.catalog)

        with_title = copy.deepcopy(layout)
        with_title["widgets"][0]["options"]["show_title"] = True
        with self.assertRaises(LayoutError):
            validate_layout(with_title, self.catalog)

    def test_panel_mapping(self) -> None:
        adapter = PanelAdapter("http://127.0.0.1:8800", self.store)
        try:
            adapter.apply(
                {
                    "service": "active",
                    "container_uptime": "5分钟",
                    "uptime": "1小时",
                    "lan_ip": "10.0.0.5",
                    "cpu_percent": 12.5,
                    "cpu_freq_mhz": 3000,
                    "metrics": {
                        "api_online": True,
                        "prefill_tps": 10.0,
                        "decode_tps": 20.0,
                        "phase": "idle",
                        "prefill_tps_avg": None,
                        "decode_tps_avg": None,
                        "prefill_peak": 30.0,
                        "decode_peak": 40.0,
                        "kv_used_tokens": 50.0,
                        "cache_hit_tokens": 0.0,
                        "requests_processing": 0,
                        "requests_deferred": 0,
                    },
                    "health": {
                        "model": "test-model",
                        "version": {"api": "1.2.3"},
                        "busy": False,
                        "slots": 2,
                        "context": 100,
                        "slot_ctx": 100,
                        "kv_pool_positions": 100,
                        "in_flight": 0,
                        "queued": 0,
                        "longest_wait_s": 0.0,
                        "busy_for_s": 0.0,
                        "drafter_default": "mtp",
                        "drafter_weights_loaded": True,
                        "prompt_cache": {"enabled": True, "cap_mb": 110},
                        "engine": {"responds": True, "probe_s": 0.5},
                        "npu": {
                            "status": "ok",
                            "loaded": ["embedding-x", "tts-y"],
                            "queued": 0,
                            "contexts": 4,
                            "passes": 2,
                        },
                    },
                    "gpu": {
                        "available": True,
                        "busy": 5,
                        "temp_c": 40,
                        "power_w": 20,
                        "vram_used_gib": 0.2,
                        "vram_total_gib": 0.5,
                        "sclk_mhz": 600,
                        "pstate": "600Mhz",
                    },
                    "mem": {"total_gib": 16, "real_available_gib": 8},
                    "disk": {"total_gib": 100, "free_gib": 50},
                    "ssd": {"life_pct": 99, "temp_c": 35, "written_tb": 1.2},
                }
            )
        finally:
            asyncio.run(adapter.close())

        self.assertEqual(self.store.values["llm.model"].value, "test-model")
        self.assertAlmostEqual(self.store.values["llm.kv_pct"].value, 50.0)
        self.assertTrue(self.store.values["tts.online"].value)
        self.assertTrue(self.store.values["embed.online"].value)
        self.assertAlmostEqual(self.store.values["host.gpu_temp_c"].value, 40.0)
        self.assertAlmostEqual(self.store.values["host.ram_used_mb"].value, 8192.0)


if __name__ == "__main__":
    unittest.main()
