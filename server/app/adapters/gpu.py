from __future__ import annotations

import asyncio
import logging
import shutil
import subprocess
import time

from ..core.store import MetricStore
from .base import Adapter

log = logging.getLogger("llmdash.gpu")

_GPU_METRICS = (
    "server.gpu_available",
    "server.gpu_util_pct",
    "server.gpu_temp_c",
    "server.gpu_power_w",
    "server.gpu_vram_used_mb",
    "server.gpu_vram_total_mb",
)


def _float(text: str) -> float | None:
    try:
        return float(text.strip())
    except (TypeError, ValueError):
        return None


class GpuAdapter(Adapter):
    name = "gpu"

    def __init__(self, store: MetricStore) -> None:
        super().__init__()
        self.store = store
        self.exe = shutil.which("nvidia-smi")

    async def poll(self) -> None:
        if not self.exe:
            raise RuntimeError("nvidia-smi 未找到")
        self.status.polls += 1
        try:
            result = await asyncio.to_thread(
                subprocess.run,
                [
                    self.exe,
                    "--query-gpu=utilization.gpu,temperature.gpu,power.draw,memory.used,memory.total",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
                timeout=4,
            )
            if result.returncode != 0:
                raise RuntimeError((result.stderr or "nvidia-smi 失败").strip()[:200])
            line = next((x for x in result.stdout.splitlines() if x.strip()), "")
            parts = [x.strip() for x in line.split(",")]
            if len(parts) < 5:
                raise RuntimeError(f"nvidia-smi 输出无法解析: {line!r}")
            updates = {
                "server.gpu_available": True,
                "server.gpu_util_pct": _float(parts[0]),
                "server.gpu_temp_c": _float(parts[1]),
                "server.gpu_power_w": _float(parts[2]),
                "server.gpu_vram_used_mb": _float(parts[3]),
                "server.gpu_vram_total_mb": _float(parts[4]),
            }
            for metric_id, value in updates.items():
                self.store.update(metric_id, value, value is not None)
            self.status.ok = True
            self.status.last_ok = time.time()
            self.status.last_error = None
        except Exception as exc:
            self.status.ok = False
            self.status.failures += 1
            self.status.last_error = f"{type(exc).__name__}: {exc}"[:240]
            for metric_id in _GPU_METRICS:
                self.store.update(metric_id, None, False)
            log.warning("gpu poll failed: %s", exc)
