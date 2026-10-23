from __future__ import annotations

import asyncio
import logging

from fastapi import WebSocket

from .store import MetricStore

log = logging.getLogger("llmdash.ws")


class ConnectionManager:
    def __init__(self) -> None:
        self._connections: dict[WebSocket, set[str] | None] = {}

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self._connections[websocket] = set()

    def disconnect(self, websocket: WebSocket) -> None:
        self._connections.pop(websocket, None)

    def set_subscription(
        self, websocket: WebSocket, ids: set[str] | None
    ) -> None:
        if websocket in self._connections:
            self._connections[websocket] = ids

    @property
    def count(self) -> int:
        return len(self._connections)

    async def broadcast(self, store: MetricStore) -> None:
        for websocket, ids in list(self._connections.items()):
            try:
                await websocket.send_json(store.snapshot(ids))
            except Exception:
                self.disconnect(websocket)

    async def broadcast_event(self, message: dict) -> None:
        for websocket in list(self._connections):
            try:
                await websocket.send_json(message)
            except Exception:
                self.disconnect(websocket)

    async def run(self, store: MetricStore, interval: float = 1.0) -> None:
        while True:
            await asyncio.sleep(interval)
            await self.broadcast(store)
