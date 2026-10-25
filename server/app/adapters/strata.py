"""Strata（Niko1221/Strata）服务端适配器。

官方来源：`docs/DETAILS.md#using-it`，字段真值在 `serve/server.py`。
只读两个端点：

* `GET /metrics`（JSON）—— Monitor 页的数据源，含 `engine` / `live` /
  `totals` / `hardware` / `hardware_static` / `history`。
* `GET /v1/status` —— `uptime_s`、引擎版本、并发与上下文。

注意 `/metrics/prometheus` 不含主机指标，因此不使用它。配了 API key 时
Strata 的 `/metrics`、`/status`、`/props`、`/v1/models` 全部要求 Bearer。
"""

from __future__ import annotations

import logging
import time
from typing import Any

import httpx

from ..core.store import MetricStore
from .base import Adapter

log = logging.getLogger("esp32wifi.strata")

BYTES_PER_MB = 1024.0 * 1024.0
# Strata 官方语义：state 为 reading / generating 时引擎在干活
BUSY_STATES = {"reading", "generating"}


def _dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _num(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    return None


def _text(value: Any) -> str | None:
    if value is None or isinstance(value, (dict, list)):
        return None
    text = str(value)
    return text or None


def _seq_max(value: Any) -> float | None:
    """history.* 是 60 点采样序列，取最大值作为峰值。"""
    if not isinstance(value, list):
        return None
    values = [float(v) for v in value if _num(v) is not None]
    return max(values) if values else None


class StrataAdapter(Adapter):
    name = "strata"

    def __init__(
        self,
        base_url: str,
        store: MetricStore,
        timeout_s: float = 4.0,
        api_key: str = "",
    ) -> None:
        super().__init__()
        self.base_url = base_url.rstrip("/")
        self.store = store
        headers = {"Accept": "application/json"}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        self.client = httpx.AsyncClient(timeout=timeout_s, headers=headers)
        self._consecutive_failures = 0
        self._last_ids: set[str] = set()
        self._logged_ok = False

    async def poll(self) -> None:
        self.status.polls += 1
        try:
            metrics = await self._get_json("/metrics")
            status = await self._get_json("/v1/status")
        except Exception as exc:
            self._consecutive_failures += 1
            self.status.ok = False
            self.status.failures += 1
            self.status.last_error = f"{type(exc).__name__}: {exc}"[:240]
            if self._consecutive_failures >= 2:
                for metric_id in self._last_ids:
                    self.store.update(metric_id, None, False)
            if self._consecutive_failures == 1 or self._consecutive_failures % 10 == 0:
                log.warning("strata poll failed (%d): %s", self._consecutive_failures, exc)
            return

        self._consecutive_failures = 0
        self.apply(metrics, status)
        self.status.ok = True
        self.status.last_ok = time.time()
        self.status.last_error = None
        if not self._logged_ok:
            log.info("strata source connected: %s", self.base_url)
            self._logged_ok = True

    async def _get_json(self, path: str) -> dict[str, Any]:
        response = await self.client.get(f"{self.base_url}{path}")
        response.raise_for_status()
        data = response.json()
        return data if isinstance(data, dict) else {}

    def apply(self, metrics: dict[str, Any], status: dict[str, Any]) -> None:
        engine = _dict(metrics.get("engine"))
        live = _dict(metrics.get("live"))
        totals = _dict(metrics.get("totals"))
        hardware = _dict(metrics.get("hardware"))
        hardware_static = _dict(metrics.get("hardware_static"))
        history = _dict(metrics.get("history"))
        concurrency = _dict(status.get("concurrency"))
        context = _dict(status.get("context"))
        activity = _dict(status.get("activity"))

        state = _text(live.get("state"))
        running = _num(live.get("running"))
        if running is None:
            # 单请求模式没有 running，用 state 推一个 0/1
            running = 1.0 if state in BUSY_STATES else 0.0
        in_flight = _num(activity.get("in_flight"))
        processing = running if running > 0 else (in_flight or 0.0)

        queued = _num(live.get("queued")) or 0.0
        waiting = _num(live.get("waiting")) or 0.0
        deferred = queued + waiting

        max_context = _num(engine.get("max_context")) or _num(context.get("native"))
        prompt_tokens = _num(live.get("prompt_tokens"))
        generated = _num(live.get("generated"))
        kv_used: float | None = None
        kv_pct: float | None = None
        if state in BUSY_STATES and prompt_tokens is not None and max_context:
            kv_used = prompt_tokens + (generated or 0.0)
            kv_pct = min(100.0, kv_used / max_context * 100.0)

        vram_total = _num(hardware.get("gpu_mem_total"))
        if vram_total is None:
            vram_total_mib = _num(hardware_static.get("gpu_vram_total_mib"))
            vram_total = vram_total_mib if vram_total_mib is not None else None
        else:
            vram_total = vram_total / BYTES_PER_MB
        gpu_util = _num(hardware.get("gpu_util"))
        gpu_available = gpu_util is not None or (_num(hardware_static.get("gpu_count")) or 0) > 0

        updates: dict[str, Any] = {
            "llm.online": True,
            "llm.model": _text(status.get("model")) or _text(engine.get("model")),
            "llm.version": _text(status.get("engine")) or _text(engine.get("version")),
            "llm.phase": _text(live.get("phase")) or state,
            "llm.busy": processing > 0,
            "llm.decode_tps": _num(live.get("tok_s")),
            "llm.prefill_tps": _num(live.get("prefill_tok_s_mean")),
            "llm.decode_avg": _num(live.get("tok_s_mean")),
            "llm.prefill_avg": _num(live.get("prefill_tok_s_mean")),
            "llm.decode_peak": _seq_max(history.get("tok_s")),
            "llm.prefill_peak": _seq_max(history.get("prefill_tok_s_mean")),
            "llm.requests_processing": processing,
            "llm.requests_deferred": deferred,
            "llm.slots": _num(concurrency.get("serving")) or _num(live.get("slots")),
            "llm.context_len": max_context,
            "llm.kv_used_tokens": kv_used,
            "llm.kv_pct": kv_pct,
            "llm.cache_hit_tokens": _num(totals.get("reused")),
            "llm.requests_total": _num(totals.get("requests")),
            "llm.tokens_in": _num(totals.get("prompt_tokens")),
            "llm.tokens_out": _num(totals.get("output_tokens")),
            "llm.uptime_s": _num(status.get("uptime_s")),
            "host.cpu_pct": _num(hardware.get("cpu")),
            "host.ram_total_mb": _div(_num(hardware.get("ram_total")), BYTES_PER_MB),
            "host.ram_used_mb": _div(_num(hardware.get("ram_used")), BYTES_PER_MB),
            "host.gpu_available": gpu_available,
            "host.gpu_util_pct": gpu_util,
            "host.gpu_temp_c": _num(hardware.get("gpu_temp")),
            "host.gpu_power_w": _num(hardware.get("gpu_power")),
            "host.gpu_vram_used_mb": _div(_num(hardware.get("gpu_mem_used")), BYTES_PER_MB),
            "host.gpu_vram_total_mb": vram_total,
        }

        for metric_id, value in updates.items():
            self.store.update(metric_id, value, value is not None)
        self._last_ids = set(updates)

    async def close(self) -> None:
        await self.client.aclose()


def _div(value: float | None, divisor: float) -> float | None:
    return None if value is None else value / divisor
