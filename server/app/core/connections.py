from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass, field

from fastapi import WebSocket

from .store import MetricStore

log = logging.getLogger("esp32wifi.ws")


@dataclass
class Client:
    websocket: WebSocket
    device_id: str | None = None
    subscriptions: set[str] | None = field(default_factory=set)

    @property
    def is_device(self) -> bool:
        return bool(self.device_id)


class ConnectionManager:
    def __init__(self) -> None:
        self._clients: dict[WebSocket, Client] = {}

    async def connect(
        self, websocket: WebSocket, device_id: str | None = None
    ) -> None:
        await websocket.accept()
        self._clients[websocket] = Client(websocket=websocket, device_id=device_id)

    def disconnect(self, websocket: WebSocket) -> None:
        self._clients.pop(websocket, None)

    def set_subscription(
        self, websocket: WebSocket, ids: set[str] | None
    ) -> None:
        client = self._clients.get(websocket)
        if client is not None:
            client.subscriptions = ids

    @property
    def count(self) -> int:
        return len(self._clients)

    @property
    def device_count(self) -> int:
        return sum(1 for c in self._clients.values() if c.is_device)

    def device_online(self, device_id: str) -> bool:
        return any(c.device_id == device_id for c in self._clients.values())

    def online_devices(self) -> list[str]:
        return [c.device_id for c in self._clients.values() if c.device_id]

    async def broadcast(self, store: MetricStore) -> None:
        for client in list(self._clients.values()):
            try:
                await client.websocket.send_json(store.snapshot(client.subscriptions))
            except Exception:
                self.disconnect(client.websocket)

    async def broadcast_event(
        self, message: dict, device_id: str | None = None
    ) -> None:
        """device_id=None 表示广播给所有连接；否则发给该设备 + 所有编辑器 viewer。"""
        for client in list(self._clients.values()):
            if device_id is not None and client.device_id not in (None, device_id):
                continue
            try:
                await client.websocket.send_json(message)
            except Exception:
                self.disconnect(client.websocket)

    async def send_to_device(self, device_id: str, message: dict) -> bool:
        sent = False
        for client in list(self._clients.values()):
            if client.device_id != device_id:
                continue
            try:
                await client.websocket.send_json(message)
                sent = True
            except Exception:
                self.disconnect(client.websocket)
        return sent

    async def run(self, store: MetricStore, interval: float = 1.0) -> None:
        while True:
            await asyncio.sleep(interval)
            await self.broadcast(store)
