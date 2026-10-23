from __future__ import annotations

import logging
import socket
import time

from ..core.store import MetricStore
from .base import Adapter

log = logging.getLogger("llmdash.system")

try:
    import psutil
except ImportError:  # pragma: no cover
    psutil = None


class SystemAdapter(Adapter):
    name = "system"

    def __init__(self, store: MetricStore) -> None:
        super().__init__()
        self.store = store
        if psutil is not None:
            psutil.cpu_percent(interval=None)

    async def poll(self) -> None:
        if psutil is None:
            raise RuntimeError("psutil 未安装")
        self.status.polls += 1
        try:
            vm = psutil.virtual_memory()
            updates = {
                "server.hostname": socket.gethostname(),
                "server.cpu_pct": float(psutil.cpu_percent(interval=None)),
                "server.ram_total_mb": vm.total / 1024 / 1024,
                "server.ram_used_mb": (vm.total - vm.available) / 1024 / 1024,
                "server.uptime_s": time.time() - psutil.boot_time(),
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
            log.warning("system poll failed: %s", exc)
