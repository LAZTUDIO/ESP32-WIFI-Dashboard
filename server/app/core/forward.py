"""服务端 API 转发配置。

设备可以把传统 API 地址指向 `http://<server>:8080/api/v1/forward`，
服务端携带这里配置的密钥请求上游 API；这样设备、API、看板都只需要一个端口，
长 API Key 也只在电脑端保存。
"""

from __future__ import annotations

import json
import os
import threading
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

PROVIDERS = {
    "auto",
    "halogen",
    "gufo",
    "strata",
    "lmstudio",
    "ollama",
    "openai",
}


@dataclass
class ForwardSettings:
    enabled: bool = False
    url: str = ""
    key: str = ""
    provider: str = "auto"
    timeout_s: float = 4.0

    def public(self) -> dict[str, Any]:
        return asdict(self)


class ForwardConfig:
    def __init__(self, path: Path) -> None:
        self._path = path
        self._lock = threading.RLock()
        self._settings = ForwardSettings()
        self.load()

    def load(self) -> None:
        with self._lock:
            if not self._path.exists():
                return
            try:
                raw = json.loads(self._path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return
            try:
                self._settings = self._validate(raw, self._settings)
            except ValueError:
                pass

    def get(self) -> ForwardSettings:
        with self._lock:
            return ForwardSettings(**asdict(self._settings))

    def update(self, payload: dict[str, Any]) -> ForwardSettings:
        with self._lock:
            settings = self._validate(payload, self._settings)
            self._settings = settings
            self._save()
            return ForwardSettings(**asdict(settings))

    @staticmethod
    def _validate(
        raw: dict[str, Any], current: ForwardSettings
    ) -> ForwardSettings:
        enabled = bool(raw.get("enabled", current.enabled))
        url = str(raw.get("url", current.url) or "").strip().rstrip("/")
        key = str(raw.get("key", current.key) or "")
        provider = str(raw.get("provider", current.provider) or "auto").strip()
        try:
            timeout_s = float(raw.get("timeout_s", current.timeout_s))
        except (TypeError, ValueError) as exc:
            raise ValueError("timeout_s 必须是数字") from exc
        if provider not in PROVIDERS:
            raise ValueError(f"不支持的服务类型: {provider}")
        if url:
            parsed = urlparse(url if "://" in url else f"http://{url}")
            if parsed.scheme not in ("http", "https") or not parsed.netloc:
                raise ValueError("转发地址格式不正确")
            url = f"{parsed.scheme}://{parsed.netloc}{parsed.path}".rstrip("/")
        if enabled and not url:
            raise ValueError("启用转发前请填写上游 API 地址")
        timeout_s = min(30.0, max(1.0, timeout_s))
        return ForwardSettings(
            enabled=enabled,
            url=url,
            key=key,
            provider=provider,
            timeout_s=timeout_s,
        )

    def _save(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self._path.with_suffix(".json.tmp")
        tmp.write_text(
            json.dumps(self._settings.public(), ensure_ascii=False, indent=2)
            + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, self._path)
