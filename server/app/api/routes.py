from __future__ import annotations

import hashlib
import json
import logging
import socket
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import httpx
from fastapi import APIRouter, HTTPException, Request, Response, WebSocket, WebSocketDisconnect

from ..config import LAYOUT_DIR, SHARED_DIR
from ..core.devices import Device, DeviceRegistry
from ..core.fonts import FONT_SIZES, find_font_path, font_bytes
from ..core.resolve import resolve_layout
from ..core.validate import LayoutError, validate_layout

log = logging.getLogger("esp32wifi.api")

router = APIRouter(prefix="/api/v1")
ws_router = APIRouter()


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _load_layout(profile: str, preset: bool = False) -> dict[str, Any]:
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
    return layout


def _font_payload_for_layout(
    request: Request, layout: dict[str, Any], size: int = 16
) -> tuple[bytes, str, int, str]:
    resolved = resolve_layout(layout, request.app.state.catalog)
    font_path = find_font_path(request.app.state.settings.font_path)
    if not font_path:
        raise HTTPException(
            status_code=503,
            detail="没有可用的 CJK 字体，请在 config.json 里设置 font_path",
        )
    try:
        data, chars = font_bytes(resolved, font_path, size)
    except ImportError as exc:
        raise HTTPException(
            status_code=503, detail="字体生成需要 Pillow：pip install Pillow"
        ) from exc
    except Exception as exc:  # noqa: BLE001
        log.exception("font subset generation failed")
        raise HTTPException(status_code=500, detail=f"字体生成失败: {exc}") from exc
    return data, hashlib.sha256(data).hexdigest(), len(chars), font_path


def _font_payload(
    request: Request, profile: str, size: int = 16
) -> tuple[bytes, str, int, str]:
    catalog = request.app.state.catalog
    if profile not in catalog.profiles:
        raise HTTPException(status_code=404, detail=f"未知画布 profile: {profile}")
    return _font_payload_for_layout(request, _load_layout(profile), size)


def _font_meta_payload(
    request: Request, layout: dict[str, Any], profile: str
) -> dict[str, Any]:
    sizes = []
    for size in FONT_SIZES:
        data, sha, chars, _ = _font_payload_for_layout(request, layout, size)
        sizes.append(
            {
                "size": size,
                "sha256": sha,
                "chars": chars,
                "bytes": len(data),
            }
        )
    # 兼容旧固件：顶层字段仍返回 16px 子集
    legacy_data, legacy_sha, legacy_chars, font_path = _font_payload_for_layout(
        request, layout, 16
    )
    return {
        "profile": profile,
        "sha256": legacy_sha,
        "chars": legacy_chars,
        "size": len(legacy_data),
        "font": Path(font_path).name,
        "sizes": sizes,
    }


# ============================ 设备管理辅助 ============================


def _devices(request: Request) -> DeviceRegistry:
    return request.app.state.devices


def _device_or_404(request: Request, device_id: str) -> Device:
    device = _devices(request).get(device_id)
    if device is None:
        raise HTTPException(status_code=404, detail=f"未知设备: {device_id}")
    return device


def _device_layout(request: Request, device: Device) -> dict[str, Any]:
    try:
        return _devices(request).read_layout(device)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


def _normalize_addr(raw: str) -> tuple[str, int]:
    text = (raw or "").strip()
    if not text:
        raise HTTPException(status_code=400, detail="请填写设备 IP")
    if "://" not in text:
        text = "http://" + text
    parsed = urlparse(text)
    host = parsed.hostname
    if not host:
        raise HTTPException(status_code=400, detail="设备 IP 格式不正确")
    try:
        port = parsed.port or 80
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="设备端口不正确") from exc
    return host, port


def _device_base(host: str, port: int) -> str:
    display = f"[{host}]" if ":" in host else host
    return f"http://{display}" if port == 80 else f"http://{display}:{port}"


def _clean_api(raw: Any) -> dict[str, str]:
    if not isinstance(raw, dict):
        return {}
    return {
        "url": str(raw.get("url") or "").strip().rstrip("/"),
        "key": str(raw.get("key") or ""),
        "provider": str(raw.get("provider") or "auto").strip() or "auto",
        "host_agent": str(raw.get("host_agent") or "").strip().rstrip("/"),
    }


def _lan_ip() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def _server_base_for_host(host: str, settings: Any) -> str:
    configured = str(getattr(settings, "public_url", "") or "").strip()
    if configured:
        return configured.rstrip("/")
    host = (host or "").strip()
    if host:
        if host.startswith("["):
            name = host[1:].split("]")[0]
            port_text = host.rsplit(":", 1)[-1] if "]:" in host else ""
        else:
            name, _, port_text = host.partition(":")
        if name not in ("127.0.0.1", "localhost", "::1"):
            return f"http://{host}"
        if port_text.isdigit():
            return f"http://{_lan_ip()}:{port_text}"
    return f"http://{_lan_ip()}:{settings.port}"


def _server_base(request: Request, settings: Any) -> str:
    return _server_base_for_host(request.headers.get("host", ""), settings)


async def _probe_device(host: str, port: int) -> dict[str, Any]:
    base = _device_base(host, port)
    try:
        async with httpx.AsyncClient(timeout=4.0) as client:
            resp = await client.get(f"{base}/api/v1/info")
            resp.raise_for_status()
            info = resp.json()
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(
            status_code=502,
            detail=f"无法连接设备 {base}/api/v1/info：{exc}",
        ) from exc
    if not isinstance(info, dict) or not info.get("id"):
        raise HTTPException(status_code=502, detail="设备返回的信息缺少 id")
    return info


def _device_config(device: Device, server_base: str) -> dict[str, Any]:
    return {
        "dashboard_url": server_base,
        "device_id": device.id,
        "name": device.name,
        "profile": device.profile,
        "rot180": device.rot180,
        "api": _clean_api(device.api),
    }


async def _http_push_device(device: Device, server_base: str) -> None:
    payload = _device_config(device, server_base)
    url = f"{_device_base(device.ip, device.port)}/api/v1/config"
    try:
        async with httpx.AsyncClient(timeout=6.0) as client:
            resp = await client.post(url, json=payload)
            resp.raise_for_status()
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(
            status_code=502,
            detail=f"配置下发失败（{url}）：{exc}",
        ) from exc


async def _apply_device_config(
    request: Request, device: Device
) -> tuple[str, str]:
    """在线走 WS（管理 HTTP 已释放）；离线尝试 HTTP，失败则等设备上线后 WS 同步。"""
    server_base = _server_base(request, request.app.state.settings)
    payload = _device_config(device, server_base)
    connections = request.app.state.connections
    if connections.device_online(device.id):
        if await connections.send_to_device(
            device.id, {"type": "config", "config": payload}
        ):
            return "ws", server_base
    try:
        await _http_push_device(device, server_base)
        return "http", server_base
    except HTTPException:
        return "pending", server_base


@router.get("/health")
def health(request: Request) -> dict[str, Any]:
    aggregator = request.app.state.aggregator
    connections = request.app.state.connections
    return {
        "status": "ok",
        "version": "0.3.0",
        "clients": connections.count,
        "devices": connections.device_count,
        "devices_online": connections.online_devices(),
        "device_total": request.app.state.devices.count(),
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
        "themes": {"version": catalog.version, "themes": catalog.theme_list},
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


@router.get("/themes")
def themes(request: Request) -> dict[str, Any]:
    data = request.app.state.catalog
    return {"version": data.version, "themes": data.theme_list}


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

    layout = _load_layout(profile, preset)
    if resolved:
        layout = resolve_layout(layout, catalog)
    return layout


@router.get("/font/meta")
def font_meta(request: Request, profile: str = "320x480") -> dict[str, Any]:
    """字体子集的元信息；设备按字号逐个比对 sha256，相同就不重复下载。"""
    catalog = request.app.state.catalog
    if profile not in catalog.profiles:
        raise HTTPException(status_code=404, detail=f"未知画布 profile: {profile}")
    return _font_meta_payload(request, _load_layout(profile), profile)


@router.get("/font")
def font(request: Request, profile: str = "320x480", size: int = 16) -> Response:
    """按当前布局文字生成的 LVGL binfont；无需鉴权。"""
    if not 8 <= size <= 32:
        raise HTTPException(status_code=400, detail="字号需在 8–32 之间")
    data, sha, chars, _ = _font_payload(request, profile, size)
    log.info("font subset: profile=%s size=%d chars=%d bytes=%d sha=%s",
             profile, size, chars, len(data), sha[:12])
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "X-Font-SHA256": sha,
            "X-Font-Chars": str(chars),
            "Cache-Control": "no-store",
        },
    )


@router.get("/server-info")
def server_info(request: Request) -> dict[str, Any]:
    settings = request.app.state.settings
    return {
        "public_url": _server_base(request, settings),
        "lan_ip": _lan_ip(),
        "port": settings.port,
    }


@router.get("/devices")
def list_devices(request: Request) -> dict[str, Any]:
    registry = _devices(request)
    connections = request.app.state.connections
    return {
        "server_url": _server_base(request, request.app.state.settings),
        "online": connections.device_count,
        "devices": [
            device.public(connections.device_online(device.id))
            for device in registry.list()
        ],
    }


@router.get("/devices/{device_id}")
def get_device(device_id: str, request: Request) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    connections = request.app.state.connections
    return device.public(connections.device_online(device.id))


@router.post("/devices/connect")
async def connect_device(payload: dict[str, Any], request: Request) -> dict[str, Any]:
    """按 IP 接入设备：在线设备走 WS，未上线的设备走管理 HTTP 下发。"""
    host, port = _normalize_addr(str(payload.get("ip") or ""))
    registry = _devices(request)
    connections = request.app.state.connections

    name = str(payload.get("name") or "").strip()[:32] or None
    updates: dict[str, Any] = {}
    if "api" in payload:
        updates["api"] = _clean_api(payload.get("api"))
    if "rot180" in payload:
        updates["rot180"] = bool(payload.get("rot180"))
    if name:
        updates["name"] = name

    # 设备在线时管理 HTTP 已释放，登记表里的 IP 来自 WS 对端
    existing = next(
        (
            d
            for d in registry.list()
            if d.ip == host and d.port == port and connections.device_online(d.id)
        ),
        None,
    )
    if existing is not None:
        device = registry.update(existing.id, **updates) or existing
        try:
            registry.ensure_layout(device)
        except FileNotFoundError as exc:
            raise HTTPException(status_code=500, detail=str(exc)) from exc
        via, server_base = await _apply_device_config(request, device)
        log.info("device reconfigured via %s: %s (%s:%d)", via, device.id, host, port)
        return {
            "ok": True,
            "via": via,
            "server_url": server_base,
            "device": device.public(True),
        }

    info = await _probe_device(host, port)
    try:
        device = registry.upsert(info, host, name=name, port=port)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    if updates:
        device = registry.update(device.id, **updates) or device

    try:
        registry.ensure_layout(device)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    server_base = _server_base(request, request.app.state.settings)
    await _http_push_device(device, server_base)
    log.info("device connected: %s (%s:%d) -> %s", device.id, host, port, server_base)
    return {
        "ok": True,
        "via": "http",
        "server_url": server_base,
        "device": device.public(connections.device_online(device.id)),
    }


@router.patch("/devices/{device_id}")
async def update_device(
    device_id: str, payload: dict[str, Any], request: Request
) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    updates: dict[str, Any] = {}
    if "name" in payload:
        updates["name"] = str(payload.get("name") or "").strip()[:32] or device.name
    if "api" in payload:
        updates["api"] = _clean_api(payload.get("api"))
    if "rot180" in payload:
        updates["rot180"] = bool(payload.get("rot180"))
    device = _devices(request).update(device_id, **updates) or device

    via, server_base = await _apply_device_config(request, device)
    connections = request.app.state.connections
    return {
        "ok": True,
        "via": via,
        "server_url": server_base,
        "device": device.public(connections.device_online(device.id)),
    }


@router.post("/devices/{device_id}/push")
async def push_device(device_id: str, request: Request) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    via, server_base = await _apply_device_config(request, device)
    return {"ok": True, "via": via, "server_url": server_base}


@router.delete("/devices/{device_id}")
def delete_device(device_id: str, request: Request) -> dict[str, Any]:
    if not _devices(request).remove(device_id):
        raise HTTPException(status_code=404, detail=f"未知设备: {device_id}")
    return {"ok": True}


@router.get("/devices/{device_id}/layout")
def device_layout(
    device_id: str, request: Request, resolved: int = 0
) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    layout = _device_layout(request, device)
    if resolved:
        layout = resolve_layout(layout, request.app.state.catalog)
    return layout


@router.put("/devices/{device_id}/layout")
async def save_device_layout(
    device_id: str, layout: dict[str, Any], request: Request
) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    catalog = request.app.state.catalog
    try:
        validate_layout(layout, catalog)
    except LayoutError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    if layout.get("profile") != device.profile:
        raise HTTPException(
            status_code=400,
            detail=f"布局 profile {layout.get('profile')} 与设备 {device.profile} 不一致",
        )
    _devices(request).write_layout(device, layout)
    log.info("device layout saved: %s", device.id)
    await request.app.state.connections.broadcast_event(
        {
            "type": "layout_changed",
            "device_id": device.id,
            "profile": device.profile,
        },
        device_id=device.id,
    )
    return {"ok": True, "device_id": device.id, "layout": layout}


@router.get("/devices/{device_id}/font/meta")
def device_font_meta(device_id: str, request: Request) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    layout = _device_layout(request, device)
    return _font_meta_payload(request, layout, device.profile)


@router.get("/devices/{device_id}/font")
def device_font(
    device_id: str, request: Request, size: int = 16
) -> Response:
    if not 8 <= size <= 32:
        raise HTTPException(status_code=400, detail="字号需在 8–32 之间")
    device = _device_or_404(request, device_id)
    layout = _device_layout(request, device)
    data, sha, chars, _ = _font_payload_for_layout(request, layout, size)
    log.info(
        "device font: %s size=%d chars=%d bytes=%d sha=%s",
        device.id,
        size,
        chars,
        len(data),
        sha[:12],
    )
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "X-Font-SHA256": sha,
            "X-Font-Chars": str(chars),
            "Cache-Control": "no-store",
        },
    )


@router.post("/devices/{device_id}/reboot")
async def reboot_device(device_id: str, request: Request) -> dict[str, Any]:
    device = _device_or_404(request, device_id)
    connections = request.app.state.connections
    if await connections.send_to_device(device.id, {"type": "reboot"}):
        return {"ok": True, "via": "ws"}
    url = f"{_device_base(device.ip, device.port)}/api/v1/reboot"
    try:
        async with httpx.AsyncClient(timeout=4.0) as client:
            resp = await client.post(url)
            resp.raise_for_status()
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=502, detail=f"重启失败：{exc}") from exc
    return {"ok": True, "via": "http"}


@router.put("/layout")
async def save_layout(layout: dict[str, Any], request: Request) -> dict[str, Any]:
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
    await request.app.state.connections.broadcast_event(
        {"type": "layout_changed", "profile": profile}
    )
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
async def websocket_endpoint(
    websocket: WebSocket,
    device_id: str | None = None,
    profile: str = "320x480",
) -> None:
    manager = websocket.app.state.connections
    store = websocket.app.state.store
    registry = websocket.app.state.devices
    peer = websocket.client.host if websocket.client else ""
    await manager.connect(websocket, device_id=device_id)
    try:
        if device_id:
            device = registry.get(device_id)
            if device is None:
                try:
                    device = registry.upsert(
                        {"id": device_id, "profile": profile}, peer
                    )
                    registry.ensure_layout(device)
                    log.info("device auto-registered: %s (%s)", device_id, peer)
                except Exception as exc:  # noqa: BLE001
                    log.warning("device register failed: %s (%s)", device_id, exc)
                    await websocket.close(code=4004)
                    return
            else:
                registry.touch(device_id, peer)
            log.info("device ws connected: %s from %s", device_id, peer)
        await websocket.send_json(
            {"type": "hello", "v": 1, "device_id": device_id}
        )
        if device_id:
            device = registry.get(device_id)
            if device is not None:
                settings = websocket.app.state.settings
                base = _server_base_for_host(
                    websocket.headers.get("host", ""), settings
                )
                await websocket.send_json(
                    {"type": "config", "config": _device_config(device, base)}
                )
        while True:
            message = await websocket.receive_json()
            if not isinstance(message, dict):
                continue
            kind = message.get("type")
            if kind == "subscribe":
                raw = message.get("metrics")
                if isinstance(raw, list) and raw:
                    ids = {item for item in raw if isinstance(item, str)}
                    log.info(
                        "ws subscribe: %d metrics: %s",
                        len(ids),
                        ", ".join(sorted(ids)),
                    )
                    manager.set_subscription(websocket, ids or None)
                    await websocket.send_json(store.snapshot(ids or None))
                else:
                    log.info("ws subscribe: all metrics")
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
        if device_id:
            log.info("device ws disconnected: %s", device_id)
