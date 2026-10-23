from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

from fastapi import APIRouter, HTTPException, Request, WebSocket, WebSocketDisconnect

from ..config import LAYOUT_DIR, SHARED_DIR
from ..core.resolve import resolve_layout
from ..core.validate import LayoutError, validate_layout

log = logging.getLogger("llmdash.api")

router = APIRouter(prefix="/api/v1")
ws_router = APIRouter()


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


@router.get("/health")
def health(request: Request) -> dict[str, Any]:
    aggregator = request.app.state.aggregator
    connections = request.app.state.connections
    return {
        "status": "ok",
        "version": "0.2.0",
        "clients": connections.count,
        "sources": aggregator.sources(),
    }


@router.get("/bootstrap")
def bootstrap(request: Request) -> dict[str, Any]:
    catalog = request.app.state.catalog
    return {
        "version": catalog.version,
        "catalog": {"version": catalog.version, "metrics": catalog.metric_list},
        "widgets": {"version": catalog.version, "widgets": catalog.widget_list},
        "profiles": {"version": catalog.version, "profiles": catalog.profiles},
    }


@router.get("/catalog")
def catalog(request: Request) -> dict[str, Any]:
    data = request.app.state.catalog
    return {"version": data.version, "metrics": data.metric_list}


@router.get("/widgets")
def widgets(request: Request) -> dict[str, Any]:
    data = request.app.state.catalog
    return {"version": data.version, "widgets": data.widget_list}


@router.get("/profiles")
def profiles(request: Request) -> dict[str, Any]:
    data = request.app.state.catalog
    return {"version": data.version, "profiles": data.profiles}


@router.get("/snapshot")
def snapshot(request: Request) -> dict[str, Any]:
    return request.app.state.store.snapshot()


@router.get("/sources")
def sources(request: Request) -> list[dict[str, Any]]:
    return request.app.state.aggregator.sources()


@router.get("/layout")
def get_layout(
    request: Request,
    profile: str = "320x480",
    preset: bool = False,
    resolved: bool = False,
) -> dict[str, Any]:
    catalog = request.app.state.catalog
    if profile not in catalog.profiles:
        raise HTTPException(status_code=404, detail=f"未知画布 profile: {profile}")

    layout = None
    saved = LAYOUT_DIR / f"{profile}.json"
    if not preset and saved.exists():
        layout = _read_json(saved)
    else:
        fallback = SHARED_DIR / "presets" / f"default-{profile}.json"
        if fallback.exists():
            layout = _read_json(fallback)
    if layout is None:
        raise HTTPException(status_code=404, detail=f"没有 {profile} 的布局")

    if resolved:
        layout = resolve_layout(layout, catalog)
    return layout


@router.put("/layout")
def save_layout(layout: dict[str, Any], request: Request) -> dict[str, Any]:
    catalog = request.app.state.catalog
    try:
        validate_layout(layout, catalog)
    except LayoutError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    profile = layout["profile"]
    LAYOUT_DIR.mkdir(parents=True, exist_ok=True)
    path = LAYOUT_DIR / f"{profile}.json"
    path.write_text(
        json.dumps(layout, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    log.info("layout saved: %s", path)
    return {"ok": True, "profile": profile, "layout": layout}


@router.get("/layouts")
def list_layouts(request: Request) -> list[dict[str, Any]]:
    catalog = request.app.state.catalog
    result = []
    for profile in catalog.profiles:
        result.append(
            {
                "profile": profile,
                "saved": (LAYOUT_DIR / f"{profile}.json").exists(),
                "preset": (SHARED_DIR / "presets" / f"default-{profile}.json").exists(),
            }
        )
    return result


@ws_router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    manager = websocket.app.state.connections
    store = websocket.app.state.store
    await manager.connect(websocket)
    try:
        await websocket.send_json({"type": "hello", "v": 1})
        while True:
            message = await websocket.receive_json()
            if not isinstance(message, dict):
                continue
            kind = message.get("type")
            if kind == "subscribe":
                raw = message.get("metrics")
                if isinstance(raw, list) and raw:
                    ids = {item for item in raw if isinstance(item, str)}
                    manager.set_subscription(websocket, ids or None)
                    await websocket.send_json(store.snapshot(ids or None))
                else:
                    manager.set_subscription(websocket, None)
                    await websocket.send_json(store.snapshot())
            elif kind == "ping":
                await websocket.send_json({"type": "pong"})
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        manager.disconnect(websocket)
