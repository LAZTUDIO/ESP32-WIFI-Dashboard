"""一次性布局迁移：4 套主题 + 固定组件尺寸 + 两方向可排页。

用法：
  cd server
  .venv/bin/python -m app.core.migrate --dry-run
  .venv/bin/python -m app.core.migrate --apply

迁移会先备份到 server/data/backups/migrate-<UTC>/，重复执行应无变更。
"""

from __future__ import annotations

import argparse
import json
import logging
import shutil
import time
from pathlib import Path
from typing import Any

from ..config import DATA_DIR, LAYOUT_DIR, PROJECT_ROOT, SHARED_DIR
from .catalog import load_catalog
from .normalize import compact_single_page, dump_layout, is_normalized

log = logging.getLogger("esp32wifi.migrate")

BACKUP_ROOT = DATA_DIR / "backups"


def _targets() -> list[Path]:
    files = sorted(LAYOUT_DIR.glob("*.json"))
    files += sorted((SHARED_DIR / "presets").glob("default-*.json"))
    return files


def _page_count(layout: dict[str, Any]) -> int:
    pages = layout.get("pages")
    if isinstance(pages, list):
        return len(pages)
    return 1


def migrate_layouts(apply: bool = True) -> dict[str, Any]:
    catalog = load_catalog()
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    backup_root = BACKUP_ROOT / f"migrate-{stamp}"
    manifest: dict[str, Any] = {
        "generated_at": stamp,
        "applied": apply,
        "files": [],
    }

    for path in _targets():
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            log.warning("migrate skip %s: %s", path, exc)
            manifest["files"].append(
                {"path": str(path.relative_to(PROJECT_ROOT)), "error": str(exc)}
            )
            continue

        if is_normalized(raw, catalog):
            manifest["files"].append(
                {
                    "path": str(path.relative_to(PROJECT_ROOT)),
                    "changed": False,
                    "reason": "already normalized",
                }
            )
            continue

        migrated, dropped = compact_single_page(raw, catalog)
        if migrated is None:
            log.warning("migrate failed (cannot fit single page): %s", path)
            manifest["files"].append(
                {
                    "path": str(path.relative_to(PROJECT_ROOT)),
                    "changed": False,
                    "error": "cannot fit into one page / both orientations",
                }
            )
            continue

        changed = migrated != raw
        entry: dict[str, Any] = {
            "path": str(path.relative_to(PROJECT_ROOT)),
            "changed": changed,
            "pages_before": _page_count(raw),
            "pages_after": _page_count(migrated),
            "theme": migrated.get("theme"),
            "dropped": [
                {"id": w.get("id"), "type": w.get("type")} for w in dropped
            ],
        }

        if changed and apply:
            relative = path.relative_to(PROJECT_ROOT)
            backup_path = backup_root / relative
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, backup_path)
            path.write_text(dump_layout(migrated), encoding="utf-8")
            entry["backup"] = str(backup_path.relative_to(PROJECT_ROOT))
            log.info(
                "migrated %s: %s -> %s pages, theme=%s",
                relative,
                entry["pages_before"],
                entry["pages_after"],
                entry["theme"],
            )
        elif changed:
            log.info(
                "would migrate %s: %s -> %s pages",
                path.relative_to(PROJECT_ROOT),
                entry["pages_before"],
                entry["pages_after"],
            )
        manifest["files"].append(entry)

    if apply and any(f.get("changed") for f in manifest["files"]):
        backup_root.mkdir(parents=True, exist_ok=True)
        (backup_root / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        log.info("migration backup: %s", backup_root)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description="迁移主题与固定尺寸布局")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--apply", action="store_true", help="写入迁移结果")
    group.add_argument("--dry-run", action="store_true", help="只打印结果（默认）")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    result = migrate_layouts(apply=bool(args.apply))
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
