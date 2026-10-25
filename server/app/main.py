from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles

from .aggregator import Aggregator
from .api.routes import router, ws_router
from .config import DATA_DIR, LAYOUT_DIR, SHARED_DIR, WEB_DIR, load_settings
from .core.catalog import load_catalog
from .core.connections import ConnectionManager
from .core.devices import DeviceRegistry
from .core.forward import ForwardConfig
from .core.migrate import migrate_layouts
from .core.store import MetricStore

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logging.getLogger("httpx").setLevel(logging.WARNING)
log = logging.getLogger("esp32wifi.main")


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = load_settings()
    LAYOUT_DIR.mkdir(parents=True, exist_ok=True)
    catalog = load_catalog()
    try:
        migrate_layouts(apply=True)
    except Exception as exc:  # noqa: BLE001
        log.exception("layout migration failed: %s", exc)
    store = MetricStore(catalog)
    forward = ForwardConfig(DATA_DIR / "forward.json")
    aggregator = Aggregator(settings, store, forward)
    connections = ConnectionManager()
    devices = DeviceRegistry(
        DATA_DIR / "devices.json", LAYOUT_DIR, SHARED_DIR / "presets"
    )

    app.state.settings = settings
    app.state.catalog = catalog
    app.state.store = store
    app.state.aggregator = aggregator
    app.state.connections = connections
    app.state.devices = devices
    app.state.forward = forward

    await aggregator.start()
    await aggregator.attach_forward()
    broadcast_task = asyncio.create_task(
        connections.run(store, 1.0), name="ws-broadcast"
    )
    log.info(
        "ESP32-WIFI-Dashboard server 0.3.0 ready, panel=%s, editor=http://%s:%s/editor/",
        settings.panel_url,
        settings.host,
        settings.port,
    )
    try:
        yield
    finally:
        broadcast_task.cancel()
        await asyncio.gather(broadcast_task, return_exceptions=True)
        await aggregator.stop()


app = FastAPI(title="ESP32-WIFI-Dashboard Server", version="0.3.0", lifespan=lifespan)


@app.middleware("http")
async def no_store_api(request: Request, call_next):
    """目录/快照等 API 不缓存，避免编辑器加载到重启前的旧组件列表。"""
    response = await call_next(request)
    if request.url.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store"
    elif request.url.path.startswith("/editor/"):
        response.headers["Cache-Control"] = "no-cache"
    return response


app.include_router(router)
app.include_router(ws_router)
app.mount("/console", StaticFiles(directory=str(WEB_DIR / "console"), html=True), name="console")
app.mount("/editor", StaticFiles(directory=str(WEB_DIR), html=True), name="editor")


@app.get("/")
def root() -> RedirectResponse:
    return RedirectResponse("/console/")


if __name__ == "__main__":
    import uvicorn

    cfg = load_settings()
    uvicorn.run(app, host=cfg.host, port=cfg.port)
