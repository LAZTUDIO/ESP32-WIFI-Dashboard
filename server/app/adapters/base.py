from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass


@dataclass
class AdapterStatus:
    name: str
    ok: bool = False
    last_ok: float | None = None
    last_error: str | None = None
    polls: int = 0
    failures: int = 0


class Adapter(ABC):
    name = "adapter"

    def __init__(self) -> None:
        self.status = AdapterStatus(name=self.name)

    @abstractmethod
    async def poll(self) -> None:
        raise NotImplementedError

    async def close(self) -> None:
        return None
