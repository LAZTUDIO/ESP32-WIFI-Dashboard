from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles

from .aggregator import Aggregator
from .api.routes import router, ws_router
from .config import LAYOUT_DIR, WEB_DIR, load_settings
from .core.catalog import load_catalog
from .core.connections import ConnectionManager
from .core.store import MetricStore

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logging.getLogger("httpx").setLevel(logging.WARNING)
log = logging.getLogger("llmdash.main")


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = load_settings()
    LAYOUT_DIR.mkdir(parents=True, exist_ok=True)
    catalog = load_catalog()
    store = MetricStore(catalog)
    aggregator = Aggregator(settings, store)
    connections = ConnectionManager()

    app.state.settings = settings
    app.state.catalog = catalog
    app.state.store = store
    app.state.aggregator = aggregator
    app.state.connections = connections

    await aggregator.start()
    broadcast_task = asyncio.create_task(
        connections.run(store, 1.0), name="ws-broadcast"
    )
    log.info(
        "LLM Dashboard server 0.2.0 ready, panel=%s, editor=http://%s:%s/editor/",
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


app = FastAPI(title="LLM Dashboard Server", version="0.2.0", lifespan=lifespan)
app.include_router(router)
app.include_router(ws_router)
app.mount("/editor", StaticFiles(directory=str(WEB_DIR), html=True), name="editor")


@app.get("/")
def root() -> RedirectResponse:
    return RedirectResponse("/editor/")


if __name__ == "__main__":
    import uvicorn

    cfg = load_settings()
    uvicorn.run(app, host=cfg.host, port=cfg.port)
