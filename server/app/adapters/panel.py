from __future__ import annotations

import logging
import re
import time
from typing import Any

import httpx

from ..core.store import MetricStore
from .base import Adapter

log = logging.getLogger("esp32wifi.panel")

_DURATION_RE = re.compile(r"(\d+)\s*(天|小时|分|秒)")
_UNIT_SECONDS = {"天": 86400, "小时": 3600, "分": 60, "秒": 1}


def parse_duration(value: Any) -> int | None:
    if not isinstance(value, str):
        return None
    total = 0
    for amount, unit in _DURATION_RE.findall(value):
        total += int(amount) * _UNIT_SECONDS[unit]
    return total or None


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
    return str(value)


def _bool(value: Any) -> bool | None:
    return value if isinstance(value, bool) else None


class PanelAdapter(Adapter):
    name = "panel"

    def __init__(
        self, base_url: str, store: MetricStore, timeout_s: float = 4.0
    ) -> None:
        super().__init__()
        self.base_url = base_url.rstrip("/")
        self.store = store
        self.client = httpx.AsyncClient(timeout=timeout_s)
        self._consecutive_failures = 0
        self._last_ids: set[str] = set()
        self._logged_ok = False

    async def poll(self) -> None:
        self.status.polls += 1
        try:
            response = await self.client.get(f"{self.base_url}/api/status")
            response.raise_for_status()
            data = response.json()
        except Exception as exc:
            self._consecutive_failures += 1
            self.status.ok = False
            self.status.failures += 1
            self.status.last_error = f"{type(exc).__name__}: {exc}"[:240]
            if self._consecutive_failures >= 2:
                for metric_id in self._last_ids:
                    self.store.update(metric_id, None, False)
            if self._consecutive_failures == 1 or self._consecutive_failures % 10 == 0:
                log.warning(
                    "panel poll failed (%d): %s", self._consecutive_failures, exc
                )
            return

        self._consecutive_failures = 0
        self.apply(data)
        self.status.ok = True
        self.status.last_ok = time.time()
        self.status.last_error = None
        if not self._logged_ok:
            log.info("panel source connected: %s", self.base_url)
            self._logged_ok = True

    def apply(self, data: dict[str, Any]) -> None:
        health = _dict(data.get("health"))
        metrics = _dict(data.get("metrics"))
        gpu = _dict(data.get("gpu"))
        mem = _dict(data.get("mem"))
        disk = _dict(data.get("disk"))
        ssd = _dict(data.get("ssd"))
        npu = _dict(health.get("npu"))
        version = _dict(health.get("version"))
        prompt_cache = _dict(health.get("prompt_cache"))
        engine = _dict(health.get("engine"))

        service = _text(data.get("service"))
        api_online = _bool(metrics.get("api_online"))
        online: bool | None = None
        if service is not None:
            online = service == "active" and api_online is not False
        elif api_online is not None:
            online = api_online

        used = _num(metrics.get("kv_used_tokens"))
        pool = _num(health.get("kv_pool_positions"))
        slots = _num(health.get("slots"))
        slot_ctx = _num(health.get("slot_ctx"))
        if pool is None and slots is not None and slot_ctx is not None:
            pool = slots * slot_ctx
        kv_pct = (used / pool * 100.0) if used is not None and pool else None

        processing = _num(metrics.get("requests_processing"))
        if processing is None:
            processing = _num(health.get("in_flight"))
        deferred = _num(metrics.get("requests_deferred"))
        if deferred is None:
            deferred = _num(health.get("queued"))

        loaded = npu.get("loaded") if isinstance(npu.get("loaded"), list) else []
        loaded_models = ", ".join(str(x) for x in loaded) if loaded else None
        npu_status = _text(npu.get("status"))
        tts_loaded = (
            any("tts" in str(model).lower() for model in loaded) if loaded else False
        )
        embed_loaded = (
            any("embed" in str(model).lower() for model in loaded) if loaded else False
        )
        tts_online = (
            (npu_status == "ok" and tts_loaded) if npu_status is not None else None
        )
        embed_online = (
            (npu_status == "ok" and embed_loaded) if npu_status is not None else None
        )

        gpu_available = _bool(gpu.get("available"))
        gpu_values: dict[str, Any] = {"host.gpu_available": gpu_available}
        gpu_metric_ids = (
            "host.gpu_util_pct",
            "host.gpu_temp_c",
            "host.gpu_power_w",
            "host.gpu_vram_used_mb",
            "host.gpu_vram_total_mb",
            "host.gpu_sclk_mhz",
            "host.gpu_pstate",
        )
        if gpu_available:
            vram_used = _num(gpu.get("vram_used_gib"))
            vram_total = _num(gpu.get("vram_total_gib"))
            gpu_values.update(
                {
                    "host.gpu_util_pct": _num(gpu.get("busy")),
                    "host.gpu_temp_c": _num(gpu.get("temp_c")),
                    "host.gpu_power_w": _num(gpu.get("power_w")),
                    "host.gpu_vram_used_mb": (
                        vram_used * 1024.0 if vram_used is not None else None
                    ),
                    "host.gpu_vram_total_mb": (
                        vram_total * 1024.0 if vram_total is not None else None
                    ),
                    "host.gpu_sclk_mhz": _num(gpu.get("sclk_mhz")),
                    "host.gpu_pstate": _text(gpu.get("pstate")),
                }
            )
        else:
            for metric_id in gpu_metric_ids:
                gpu_values[metric_id] = None

        mem_total = _num(mem.get("total_gib"))
        mem_avail = _num(mem.get("real_available_gib"))
        if mem_avail is None:
            mem_avail = _num(mem.get("memavailable_gib"))

        updates: dict[str, Any] = {
            "llm.online": online,
            "llm.model": _text(health.get("model")) or _text(data.get("model")),
            "llm.version": _text(version.get("api")),
            "llm.phase": _text(metrics.get("phase")),
            "llm.busy": _bool(health.get("busy")),
            "llm.decode_tps": _num(metrics.get("decode_tps")),
            "llm.prefill_tps": _num(metrics.get("prefill_tps")),
            "llm.decode_avg": _num(metrics.get("decode_tps_avg")),
            "llm.prefill_avg": _num(metrics.get("prefill_tps_avg")),
            "llm.decode_peak": _num(metrics.get("decode_peak")),
            "llm.prefill_peak": _num(metrics.get("prefill_peak")),
            "llm.requests_processing": processing,
            "llm.requests_deferred": deferred,
            "llm.slots": slots,
            "llm.context_len": _num(health.get("context")) or slot_ctx,
            "llm.kv_pool_positions": pool,
            "llm.kv_used_tokens": used,
            "llm.kv_pct": kv_pct,
            "llm.cache_hit_tokens": _num(metrics.get("cache_hit_tokens")),
            "llm.cache_cap_mb": _num(prompt_cache.get("cap_mb")),
            "llm.cache_enabled": _bool(prompt_cache.get("enabled")),
            "llm.longest_wait_s": _num(health.get("longest_wait_s")),
            "llm.busy_for_s": _num(health.get("busy_for_s")),
            "llm.engine_responds": _bool(engine.get("responds")),
            "llm.engine_probe_s": _num(engine.get("probe_s")),
            "llm.uptime_s": parse_duration(data.get("container_uptime")),
            "llm.drafter": _text(health.get("drafter_default")),
            "llm.drafter_loaded": _bool(health.get("drafter_weights_loaded")),
            "tts.online": tts_online,
            "tts.loaded": tts_loaded if npu_status is not None else None,
            "tts.queue": _num(npu.get("queued")),
            "tts.contexts": _num(npu.get("contexts")),
            "embed.online": embed_online,
            "embed.loaded": embed_loaded if npu_status is not None else None,
            "embed.queue": _num(npu.get("queued")),
            "embed.contexts": _num(npu.get("contexts")),
            "npu.status": npu_status,
            "npu.passes": _num(npu.get("passes")),
            "npu.loaded_models": loaded_models,
            "host.lan_ip": _text(data.get("lan_ip")),
            "host.uptime_s": parse_duration(data.get("uptime")),
            "host.service": service,
            "host.container": _text(data.get("container")),
            "host.cpu_pct": _num(data.get("cpu_percent")),
            "host.cpu_freq_mhz": _num(data.get("cpu_freq_mhz")),
            "host.ram_total_mb": (
                mem_total * 1024.0 if mem_total is not None else None
            ),
            "host.ram_used_mb": (
                (mem_total - mem_avail) * 1024.0
                if mem_total is not None and mem_avail is not None
                else None
            ),
            "host.ram_available_mb": (
                mem_avail * 1024.0 if mem_avail is not None else None
            ),
            "host.disk_total_gb": _num(disk.get("total_gib")),
            "host.disk_free_gb": _num(disk.get("free_gib")),
            "host.ssd_life_pct": _num(ssd.get("life_pct")),
            "host.ssd_temp_c": _num(ssd.get("temp_c")),
            "host.ssd_written_tb": _num(ssd.get("written_tb")),
        }
        updates.update(gpu_values)

        for metric_id, value in updates.items():
            self.store.update(metric_id, value, value is not None)
        self._last_ids = set(updates)

    async def close(self) -> None:
        await self.client.aclose()
