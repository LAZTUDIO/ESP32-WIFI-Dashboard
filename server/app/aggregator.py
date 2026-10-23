from __future__ import annotations

import asyncio
import logging
import shutil

from .adapters.base import Adapter
from .adapters.gpu import GpuAdapter
from .adapters.panel import PanelAdapter
from .adapters.system import SystemAdapter
from .config import Settings
from .core.store import MetricStore

log = logging.getLogger("llmdash.aggregator")


class Aggregator:
    def __init__(self, settings: Settings, store: MetricStore) -> None:
        self.settings = settings
        self.store = store
        self._entries: list[tuple[Adapter, float]] = []
        self._tasks: list[asyncio.Task] = []

        if settings.panel_url:
            self._entries.append(
                (
                    PanelAdapter(
                        settings.panel_url, store, settings.panel_timeout_s
                    ),
                    settings.panel_interval_s,
                )
            )
        if settings.enable_system:
            self._entries.append((SystemAdapter(store), settings.system_interval_s))
        if settings.enable_gpu:
            if shutil.which("nvidia-smi"):
                self._entries.append((GpuAdapter(store), settings.gpu_interval_s))
            else:
                log.warning(
                    "enable_gpu=true 但未找到 nvidia-smi，服务端 GPU 适配器跳过"
                )

    async def start(self) -> None:
        for adapter, interval in self._entries:
            task = asyncio.create_task(
                self._loop(adapter, interval), name=f"adapter-{adapter.name}"
            )
            self._tasks.append(task)

    async def _loop(self, adapter: Adapter, interval: float) -> None:
        while True:
            try:
                await adapter.poll()
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("adapter %s poll crashed", adapter.name)
            await asyncio.sleep(max(0.2, interval))

    async def stop(self) -> None:
        for task in self._tasks:
            task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        for adapter, _ in self._entries:
            try:
                await adapter.close()
            except Exception:
                pass

    def sources(self) -> list[dict]:
        result = []
        for adapter, interval in self._entries:
            status = adapter.status
            result.append(
                {
                    "name": status.name,
                    "ok": status.ok,
                    "last_ok": status.last_ok,
                    "last_error": status.last_error,
                    "polls": status.polls,
                    "failures": status.failures,
                    "interval_s": interval,
                }
            )
        return result
