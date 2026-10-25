from __future__ import annotations

import asyncio
import logging
import shutil
from typing import Any

import httpx

from .adapters.base import Adapter
from .adapters.gpu import GpuAdapter
from .adapters.panel import PanelAdapter
from .adapters.strata import StrataAdapter
from .adapters.system import SystemAdapter
from .config import Settings
from .core.store import MetricStore

log = logging.getLogger("esp32wifi.aggregator")


class Aggregator:
    def __init__(
        self, settings: Settings, store: MetricStore, forward: Any = None
    ) -> None:
        self.settings = settings
        self.store = store
        self.forward = forward
        self._entries: list[tuple[Adapter, float]] = []
        self._tasks: list[asyncio.Task] = []
        self._mirror: tuple[Adapter, asyncio.Task] | None = None

        if settings.panel_url:
            self._entries.append(
                (
                    PanelAdapter(
                        settings.panel_url, store, settings.panel_timeout_s
                    ),
                    settings.panel_interval_s,
                )
            )
        if settings.strata_url:
            if settings.panel_url:
                log.warning(
                    "panel_url 与 strata_url 同时启用，两者写同一批 llm.*/host.* 测点，"
                    "以最后写入为准"
                )
            self._entries.append(
                (
                    StrataAdapter(
                        settings.strata_url,
                        store,
                        settings.strata_timeout_s,
                        settings.strata_key,
                    ),
                    settings.strata_interval_s,
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

    async def attach_forward(self) -> None:
        """转发模式下服务端只做透明代理，store 里没有任何引擎指标，编辑器会满屏
        `--`。没有单独配置上游时，按转发上游镜像一份进 store：面板口径用
        PanelAdapter，Strata 用 StrataAdapter；halogen / gufo 等引擎只有屏幕侧
        解析器，服务端不镜像。可重复调用（转发设置变更后重新挂载）。"""
        await self.detach_forward()
        if self.forward is None:
            return
        if self.settings.panel_url or self.settings.strata_url:
            return
        forward = self.forward.get()
        if not forward.enabled or not forward.url:
            return
        adapter = await _detect_forward_adapter(forward, self.store)
        if adapter is None:
            log.info(
                "转发上游 %s 无服务端适配器（halogen / gufo 等由屏幕直采），编辑器不镜像",
                forward.url,
            )
            return
        interval = self.settings.panel_interval_s
        task = asyncio.create_task(
            self._loop(adapter, interval), name=f"adapter-{adapter.name}"
        )
        self._entries.append((adapter, interval))
        self._tasks.append(task)
        self._mirror = (adapter, task)
        log.info("转发上游已镜像到服务端：%s -> %s", forward.url, adapter.name)

    async def detach_forward(self) -> None:
        if not self._mirror:
            return
        adapter, task = self._mirror
        self._mirror = None
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        try:
            await adapter.close()
        except Exception:
            pass
        self._entries = [entry for entry in self._entries if entry[0] is not adapter]
        self._tasks = [item for item in self._tasks if item is not task]

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


async def _detect_forward_adapter(
    forward: Any, store: MetricStore
) -> Adapter | None:
    """按端点特征判断转发上游的类型，启动时只探测一次。"""
    base = forward.url.rstrip("/")
    headers = {"Accept": "application/json"}
    if forward.key:
        headers["Authorization"] = f"Bearer {forward.key}"
    try:
        async with httpx.AsyncClient(timeout=min(3.0, forward.timeout_s)) as client:
            if forward.provider == "strata":
                return StrataAdapter(base, store, forward.timeout_s, forward.key)
            try:
                response = await client.get(f"{base}/api/status", headers=headers)
                if response.status_code == 200 and isinstance(response.json(), dict):
                    return PanelAdapter(base, store, forward.timeout_s)
            except Exception:  # noqa: BLE001
                pass
            try:
                response = await client.get(f"{base}/v1/status", headers=headers)
                if response.status_code == 200 and isinstance(response.json(), dict):
                    return StrataAdapter(base, store, forward.timeout_s, forward.key)
            except Exception:  # noqa: BLE001
                pass
    except Exception as exc:  # noqa: BLE001
        log.warning("转发上游探测失败：%s", exc)
    return None
