from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = SERVER_DIR.parent
SHARED_DIR = PROJECT_ROOT / "shared"
WEB_DIR = SERVER_DIR / "web"
DATA_DIR = SERVER_DIR / "data"
LAYOUT_DIR = DATA_DIR / "layouts"


@dataclass
class Settings:
    panel_url: str = "http://127.0.0.1:8800"
    panel_interval_s: float = 1.0
    panel_timeout_s: float = 4.0
    enable_system: bool = True
    system_interval_s: float = 2.0
    enable_gpu: bool = False
    gpu_interval_s: float = 5.0
    host: str = "0.0.0.0"
    port: int = 8080
    font_path: str = ""          # 可选的 CJK 字体文件；留空自动探测系统字体


def load_settings() -> Settings:
    for name in ("config.json", "config.example.json"):
        path = SERVER_DIR / name
        if not path.exists():
            continue
        raw = json.loads(path.read_text(encoding="utf-8"))
        allowed = set(Settings.__dataclass_fields__)
        return Settings(**{k: v for k, v in raw.items() if k in allowed})
    return Settings()
