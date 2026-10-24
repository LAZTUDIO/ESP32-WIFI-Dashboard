"""多屏设备注册表。

一台服务端可以管理多块 ESP32 屏。设备身份由固件按 MAC 生成（如 esp32-a1b2c3），
服务端只负责记住 IP、备注名、API 设置和每块屏自己的布局。在线状态不落盘，
由 WebSocket 连接表实时推导。
"""

from __future__ import annotations

import json
import os
import re
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,48}$")


@dataclass
class Device:
    id: str
    name: str
    ip: str
    port: int = 80
    profile: str = "320x480"
    api: dict[str, Any] = field(default_factory=dict)
    rot180: bool = False
    runtime: dict[str, Any] = field(default_factory=dict)
    created_at: float = 0.0
    last_seen: float = 0.0
    last_info: dict[str, Any] = field(default_factory=dict)

    def public(self, online: bool) -> dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "ip": self.ip,
            "port": self.port,
            "profile": self.profile,
            "api": self.api,
            "rot180": self.rot180,
            "runtime": self.runtime,
            "online": online,
            "created_at": self.created_at,
            "last_seen": self.last_seen,
            "last_info": self.last_info,
        }


class DeviceRegistry:
    def __init__(
        self,
        path: Path,
        layout_dir: Path,
        preset_dir: Path,
    ) -> None:
        self._path = path
        self._layout_dir = layout_dir
        self._preset_dir = preset_dir
        self._lock = threading.RLock()
        self._devices: dict[str, Device] = {}
        self.load()

    # ---------------- 持久化 ----------------

    def load(self) -> None:
        with self._lock:
            self._devices.clear()
            if not self._path.exists():
                return
            try:
                raw = json.loads(self._path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return
            for item in raw.get("devices", []):
                try:
                    device = Device(
                        id=str(item["id"]),
                        name=str(item.get("name") or item["id"]),
                        ip=str(item.get("ip") or ""),
                        port=int(item.get("port") or 80),
                        profile=str(item.get("profile") or "320x480"),
                        api=dict(item.get("api") or {}),
                        rot180=bool(item.get("rot180", False)),
                        runtime=dict(item.get("runtime") or {}),
                        created_at=float(item.get("created_at") or 0),
                        last_seen=float(item.get("last_seen") or 0),
                        last_info=dict(item.get("last_info") or {}),
                    )
                except (KeyError, TypeError, ValueError):
                    continue
                if _ID_RE.match(device.id):
                    self._devices[device.id] = device

    def _save(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "version": 1,
            "devices": [
                {
                    "id": d.id,
                    "name": d.name,
                    "ip": d.ip,
                    "port": d.port,
                    "profile": d.profile,
                    "api": d.api,
                    "rot180": d.rot180,
                    "runtime": d.runtime,
                    "created_at": d.created_at,
                    "last_seen": d.last_seen,
                    "last_info": d.last_info,
                }
                for d in sorted(self._devices.values(), key=lambda x: x.created_at)
            ],
        }
        tmp = self._path.with_suffix(".json.tmp")
        tmp.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, self._path)

    # ---------------- 查询 ----------------

    def get(self, device_id: str) -> Device | None:
        with self._lock:
            device = self._devices.get(device_id)
            return None if device is None else Device(**device.__dict__)

    def list(self) -> list[Device]:
        with self._lock:
            items = sorted(self._devices.values(), key=lambda x: x.created_at)
            return [Device(**d.__dict__) for d in items]

    def count(self) -> int:
        with self._lock:
            return len(self._devices)

    def next_name(self) -> str:
        with self._lock:
            used = {d.name for d in self._devices.values()}
        index = 1
        while f"屏幕 {index}" in used:
            index += 1
        return f"屏幕 {index}"

    # ---------------- 修改 ----------------

    def upsert(
        self,
        info: dict[str, Any],
        ip: str,
        name: str | None = None,
        port: int = 80,
    ) -> Device:
        device_id = str(info.get("id") or "").strip()
        if not _ID_RE.match(device_id):
            raise ValueError("设备未上报合法的 id")
        profile = str(info.get("profile") or "320x480")
        now = time.time()
        with self._lock:
            device = self._devices.get(device_id)
            if device is None:
                device = Device(
                    id=device_id,
                    name=name or self.next_name(),
                    ip=ip,
                    port=port,
                    profile=profile,
                    api={},
                    created_at=now,
                    last_seen=now,
                )
                self._devices[device_id] = device
            else:
                device.ip = ip or device.ip
                device.port = port or device.port
                device.profile = profile or device.profile
                device.last_seen = now
                if name:
                    device.name = name
            device.last_info = dict(info)
            self._save()
            return Device(**device.__dict__)

    def update(self, device_id: str, **fields: Any) -> Device | None:
        with self._lock:
            device = self._devices.get(device_id)
            if device is None:
                return None
            for key in ("name", "ip", "port", "profile", "api", "rot180", "runtime"):
                if key in fields and fields[key] is not None:
                    setattr(device, key, fields[key])
            self._save()
            return Device(**device.__dict__)

    def touch(self, device_id: str, ip: str = "") -> None:
        with self._lock:
            device = self._devices.get(device_id)
            if device is None:
                return
            device.last_seen = time.time()
            if ip and ip not in ("127.0.0.1", "::1"):
                device.ip = ip
            self._save()

    def remove(self, device_id: str) -> bool:
        with self._lock:
            if device_id not in self._devices:
                return False
            del self._devices[device_id]
            self._save()
            return True

    # ---------------- 每设备布局 ----------------

    def layout_path(self, device: Device) -> Path:
        if not _ID_RE.match(device.id):
            raise ValueError("非法设备 id")
        return self._layout_dir / f"{device.id}.json"

    def ensure_layout(self, device: Device) -> Path:
        """设备第一次接入时，从模板复制一份独立布局；之后各自修改互不影响。"""
        self._layout_dir.mkdir(parents=True, exist_ok=True)
        target = self.layout_path(device)
        if target.exists():
            return target
        sources = [
            self._layout_dir / f"{device.profile}.json",
            self._preset_dir / f"default-{device.profile}.json",
        ]
        for source in sources:
            if not source.exists():
                continue
            layout = json.loads(source.read_text(encoding="utf-8"))
            target.write_text(
                json.dumps(layout, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            return target
        raise FileNotFoundError(f"没有 {device.profile} 的布局模板")

    def read_layout(self, device: Device) -> dict[str, Any]:
        path = self.ensure_layout(device)
        return json.loads(path.read_text(encoding="utf-8"))

    def write_layout(self, device: Device, layout: dict[str, Any]) -> Path:
        path = self.layout_path(device)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(layout, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        return path
