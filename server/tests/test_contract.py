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
from app.adapters.strata import StrataAdapter  # noqa: E402
from app.core.catalog import load_catalog  # noqa: E402
from app.core.resolve import project_layout, resolve_layout  # noqa: E402
from app.core.normalize import normalize_layout  # noqa: E402
from app.core.store import MetricStore  # noqa: E402
from app.core.validate import LayoutError, validate_layout  # noqa: E402

PRESETS = SERVER_DIR.parent / "shared" / "presets"


def page_widgets(layout: dict, index: int = 0) -> list[dict]:
    return layout["pages"][index]["widgets"]


def all_widgets(layout: dict) -> list[dict]:
    return [w for page in layout.get("pages", []) for w in page.get("widgets", [])]


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

    def test_examples_validate(self) -> None:
        """examples/ 下的示例布局（含横屏 alt 坐标）必须双向可排布。

        统一走加载路径 `normalize_layout`（v1 / 多页 / 尺寸吸附都在这里收敛），
        再校验。`layout-320x480.json` 是旧版两页样例，压成单页放不下，
        归一化会返回 None —— 这类历史样例跳过而不是判失败。
        """
        examples = SERVER_DIR.parent / "examples"
        checked = 0
        for path in sorted(examples.glob("*.json")):
            with self.subTest(example=path.name):
                layout = json.loads(path.read_text(encoding="utf-8"))
                normalized = normalize_layout(layout, self.catalog)
                if normalized is None:
                    continue
                validate_layout(normalized, self.catalog)
                checked += 1
        self.assertGreater(checked, 0, "examples/ 下没有可归一化的示例布局")

    def test_showcase_covers_all_mini_types(self) -> None:
        """小组件总览示例应覆盖 widgets.json 里全部 generation 2 类型。"""
        widgets_doc = json.loads(
            (SERVER_DIR.parent / "shared" / "catalog" / "widgets.json").read_text(
                encoding="utf-8"
            )
        )["widgets"]
        mini_types = {w["type"] for w in widgets_doc if w.get("generation") == 2}
        showcase = json.loads(
            (
                SERVER_DIR.parent / "examples" / "widgets-showcase-320x480.json"
            ).read_text(encoding="utf-8")
        )
        used = {w["type"] for w in page_widgets(showcase)}
        self.assertTrue(
            mini_types.issubset(used),
            f"示例缺少小组件类型: {sorted(mini_types - used)}",
        )

    def test_overlap_rejected(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        widgets = page_widgets(layout)
        widgets[1]["x"] = widgets[0]["x"]
        widgets[1]["y"] = widgets[0]["y"]
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

    def test_binding_type_rejected(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        page_widgets(layout)[1]["bindings"]["value"] = "llm.model"
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

    def test_size_snapped_to_allowed_option(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        page_widgets(layout)[1]["w"] = 4
        page_widgets(layout)[1]["h"] = 2
        normalized = normalize_layout(layout, self.catalog)
        self.assertIsNotNone(normalized)
        validate_layout(normalized, self.catalog)
        self.assertEqual(page_widgets(normalized)[1]["w"], 4)
        self.assertEqual(page_widgets(normalized)[1]["h"], 3)

    def test_landscape_projection_keeps_single_widget_list(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        for widget in page_widgets(layout):
            widget.pop("alt", None)
        page_widgets(layout)[0]["alt"] = {"x": 0, "y": 0}
        validate_layout(layout, self.catalog)
        projected = project_layout(copy.deepcopy(layout), "480x320", self.catalog)
        self.assertEqual(projected["profile"], "480x320")
        self.assertEqual(projected["canvas"]["cols"], 18)
        self.assertEqual(projected["canvas"]["rows"], 12)
        first = page_widgets(projected)[0]
        self.assertEqual((first["w"], first["h"]), (12, 2))
        self.assertEqual((first["x"], first["y"]), (0, 0))
        self.assertNotIn("alt", first)
        for page in projected["pages"]:
            rects = []
            for widget in page["widgets"]:
                self.assertLessEqual(widget["x"] + widget["w"], 18)
                self.assertLessEqual(widget["y"] + widget["h"], 12)
                rects.append(widget)
            for index, a in enumerate(rects):
                for b in rects[index + 1 :]:
                    overlap = (
                        a["x"] < b["x"] + b["w"]
                        and b["x"] < a["x"] + a["w"]
                        and a["y"] < b["y"] + b["h"]
                        and b["y"] < a["y"] + a["h"]
                    )
                    self.assertFalse(
                        overlap, f"{a['id']} 与 {b['id']} 在横屏重叠"
                    )

    def test_alt_overlap_rejected(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        page_widgets(layout)[1]["alt"] = {"x": 0, "y": 0}
        page_widgets(layout)[2]["alt"] = {"x": 0, "y": 0}
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

    def test_single_page_layout_v2(self) -> None:
        layout = {
            "v": 2,
            "profile": "320x480",
            "pages": [
                {
                    "name": "主页",
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
                        {
                            "id": "p2-kpi",
                            "type": "kpi",
                            "x": 6,
                            "y": 2,
                            "w": 6,
                            "h": 3,
                            "bindings": {"value": "llm.requests_deferred"},
                            "options": {"title": "排队"},
                        },
                    ],
                }
            ],
        }
        validate_layout(layout, self.catalog)

        duplicate = copy.deepcopy(layout)
        duplicate["pages"][0]["widgets"][2]["id"] = "p1-kpi"
        with self.assertRaises(LayoutError):
            validate_layout(duplicate, self.catalog)

        too_many_pages = copy.deepcopy(layout)
        too_many_pages["pages"].append(
            copy.deepcopy(layout["pages"][0])
        )
        with self.assertRaises(LayoutError):
            validate_layout(too_many_pages, self.catalog)

    def test_theme_validation(self) -> None:
        for theme_id in ("neon-cyber", "minimal-air", "vintage-gauge", "vital-pulse"):
            self.assertIn(theme_id, self.catalog.themes)

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

        self.assertEqual(resolved["theme"], "vintage-gauge")
        self.assertEqual(resolved["theme_skin"], "vintage-gauge")
        tokens = resolved["theme_tokens"]
        self.assertEqual(tokens["bg"], "#17120C")
        self.assertEqual(tokens["accent"], "#FFB000")
        self.assertEqual(tokens["radius"], 8)

    def test_resolve_layout_metadata(self) -> None:
        layout = json.loads(
            (PRESETS / "default-320x480.json").read_text(encoding="utf-8")
        )
        resolved = resolve_layout(layout, self.catalog)
        widgets = {w["id"]: w for w in all_widgets(resolved)}

        kpi = widgets["w-dec"]["options"]
        self.assertEqual(kpi["unit"], "tok/s")
        self.assertEqual(kpi["metric_label"], "解码速度")

        server = widgets["w-server"]["options"]
        self.assertEqual(server["labels"]["cpu"], "CPU 使用率")
        self.assertEqual(server["units"]["cpu"], "%")
        self.assertEqual(server["labels"]["text"], "引擎版本")

    def test_fixed_size_rejects_nonstandard(self) -> None:
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
        with self.assertRaises(LayoutError):
            validate_layout(layout, self.catalog)

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
        self.assertAlmostEqual(self.store.values["host.gpu_temp_c"].value, 40.0)
        self.assertAlmostEqual(self.store.values["host.ram_used_mb"].value, 8192.0)

    def test_strata_mapping(self) -> None:
        """字段名取自 Strata 官方 `serve/server.py`：/metrics 的 JSON + /v1/status。"""
        adapter = StrataAdapter("http://127.0.0.1:8080", self.store)
        mb = 1024.0 * 1024.0
        try:
            adapter.apply(
                {
                    "engine": {"model": "qwen3-8", "max_context": 4096},
                    "live": {
                        "state": "generating",
                        "phase": "decode",
                        "queued": 1,
                        "waiting": 2,
                        "running": 2,
                        "slots": 4,
                        "prompt_tokens": 1000,
                        "generated": 24,
                        "tok_s": 42.5,
                        "tok_s_mean": 40.0,
                        "prefill_tok_s_mean": 900.0,
                    },
                    "totals": {"requests": 10, "reused": 512},
                    "hardware": {
                        "gpu_util": 55,
                        "gpu_mem_used": 6144 * mb,
                        "gpu_mem_total": 12288 * mb,
                        "gpu_temp": 61.0,
                        "gpu_power": 45.0,
                        "cpu": 12.5,
                        "ram_used": 8192 * mb,
                        "ram_total": 16384 * mb,
                    },
                    "hardware_static": {"gpu_count": 1, "gpu_vram_total_mib": 12288},
                    "history": {
                        "tok_s": [30.0, 55.0, 48.0],
                        "prefill_tok_s_mean": [800.0, 1200.0],
                    },
                },
                {
                    "model": "qwen3-8",
                    "engine": "0.1.50",
                    "uptime_s": 120,
                    "concurrency": {"serving": 4, "requested": 4},
                    "context": {"native": 4096},
                    "activity": {"in_flight": 2},
                },
            )
        finally:
            asyncio.run(adapter.close())

        values = self.store.values
        self.assertEqual(values["llm.model"].value, "qwen3-8")
        self.assertEqual(values["llm.version"].value, "0.1.50")
        self.assertEqual(values["llm.phase"].value, "decode")
        self.assertTrue(values["llm.busy"].value)
        self.assertAlmostEqual(values["llm.decode_tps"].value, 42.5)
        self.assertAlmostEqual(values["llm.decode_avg"].value, 40.0)
        self.assertAlmostEqual(values["llm.decode_peak"].value, 55.0)
        self.assertAlmostEqual(values["llm.prefill_peak"].value, 1200.0)
        self.assertEqual(values["llm.requests_processing"].value, 2.0)
        self.assertEqual(values["llm.requests_deferred"].value, 3.0)
        self.assertEqual(values["llm.slots"].value, 4.0)
        self.assertEqual(values["llm.cache_hit_tokens"].value, 512.0)
        self.assertEqual(values["llm.uptime_s"].value, 120.0)
        # KV 用 strata 自己的口径：在途 prompt+生成 占 context 的比例
        self.assertAlmostEqual(values["llm.kv_used_tokens"].value, 1024.0)
        self.assertAlmostEqual(values["llm.kv_pct"].value, 25.0)
        self.assertAlmostEqual(values["host.cpu_pct"].value, 12.5)
        self.assertAlmostEqual(values["host.ram_used_mb"].value, 8192.0)
        self.assertAlmostEqual(values["host.ram_total_mb"].value, 16384.0)
        self.assertAlmostEqual(values["host.gpu_temp_c"].value, 61.0)
        self.assertAlmostEqual(values["host.gpu_vram_used_mb"].value, 6144.0)
        self.assertAlmostEqual(values["host.gpu_vram_total_mb"].value, 12288.0)
        self.assertTrue(values["host.gpu_available"].value)

    def test_strata_idle_has_no_kv(self) -> None:
        """空闲时没有在途请求，KV 必须留空而不是 0，否则曲线会假装有占用。"""
        adapter = StrataAdapter("http://127.0.0.1:8080", self.store)
        try:
            adapter.apply(
                {"engine": {"max_context": 4096}, "live": {"state": "idle"}},
                {"concurrency": {}},
            )
        finally:
            asyncio.run(adapter.close())

        self.assertFalse(self.store.values["llm.kv_pct"].available)
        self.assertFalse(self.store.values["llm.kv_used_tokens"].available)
        self.assertFalse(self.store.values["llm.decode_peak"].available)
        self.assertFalse(self.store.values["llm.busy"].value)


class MetricProviderTest(unittest.TestCase):
    PROVIDERS = {"halogen", "gufo", "strata", "lmstudio", "ollama", "openai", "panel"}

    def test_every_metric_has_known_providers(self) -> None:
        catalog = json.loads(
            (SERVER_DIR.parent / "shared" / "catalog" / "metrics.json").read_text(
                encoding="utf-8"
            )
        )["metrics"]
        for metric in catalog:
            self.assertIsInstance(
                metric.get("providers"), list, msg=f"{metric['id']} 缺少 providers"
            )
            self.assertIn(
                metric.get("source"),
                ("api", "server"),
                msg=f"{metric['id']} source 非法",
            )
            for provider in metric["providers"]:
                self.assertIn(provider, self.PROVIDERS, msg=f"{metric['id']} 未知类型")

    def test_api_metrics_carry_provenance(self) -> None:
        """引擎 API 测点必须逐条注明原始遥测出处，防止再混进取不到的点位。"""
        catalog = json.loads(
            (SERVER_DIR.parent / "shared" / "catalog" / "metrics.json").read_text(
                encoding="utf-8"
            )
        )["metrics"]
        for metric in catalog:
            if metric.get("source") != "api":
                continue
            provenance = str(metric.get("api") or "")
            self.assertTrue(
                len(provenance) >= 8,
                msg=f"{metric['id']} 缺少 api 原始遥测出处",
            )
            self.assertTrue(
                metric.get("providers"),
                msg=f"{metric['id']} 标为 api 但没有交付它的引擎",
            )


class ForwardConfigTest(unittest.TestCase):
    def test_roundtrip_and_validation(self) -> None:
        import tempfile

        from app.core.forward import ForwardConfig

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "forward.json"
            cfg = ForwardConfig(path)
            self.assertFalse(cfg.get().enabled)
            with self.assertRaises(ValueError):
                cfg.update({"enabled": True, "url": ""})
            with self.assertRaises(ValueError):
                cfg.update({"enabled": True, "url": "http://127.0.0.1:8000", "provider": "nope"})
            saved = cfg.update(
                {
                    "enabled": True,
                    "url": "http://127.0.0.1:8000/v1/",
                    "key": "secret",
                    "provider": "lmstudio",
                }
            )
            self.assertEqual(saved.url, "http://127.0.0.1:8000/v1")
            reloaded = ForwardConfig(path).get()
            self.assertTrue(reloaded.enabled)
            self.assertEqual(reloaded.key, "secret")
            self.assertEqual(reloaded.provider, "lmstudio")


if __name__ == "__main__":
    unittest.main()
