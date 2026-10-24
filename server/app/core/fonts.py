"""LVGL 二进制字体子集生成。

把布局里实际会显示的文字（标题、标签、单位、状态文案）收集起来，用系统 CJK
字体渲染成 LVGL 的 binfont 格式，设备端 `lv_binfont_create_ex()` 按需加载。
这样布局可以写任意中文，而不用把整套字库塞进 Flash。
"""

from __future__ import annotations

import hashlib
import struct
import threading
from pathlib import Path
from typing import Any, Iterable

# ui_dynamic.c / ui_widgets.c 里写死的中文文案 + 常用符号
BASE_TEXT = (
    "状态条大数字迷你曲线服务器指标列表组件配网模式连接热点后打开浏览器"
    "密码保存后设备自动重启并连接在线离线解码预填充失败等待未知"
    "温度速度功率内存显存并发排队命中缓存运行时长当前均值峰值模型"
    "版本服务地址密钥已设置未设置无数据是否"
    "—…℃%·×→：；，。！？（）【】《》"
)

FONT_CANDIDATES = (
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
)

_LOCK = threading.Lock()
_CACHE: dict[tuple[str, int, int, str], bytes] = {}
_FORMAT_VERSION = 3   # 生成器格式/参数变更时递增，避免命中旧缓存

# 与 Web 设计字号一致：标题/说明 11、服务器卡 13、指标列表 S/M/L 11/13/15
FONT_SIZES = (11, 13, 15)


def find_font_path(explicit: str = "") -> str | None:
    if explicit:
        return explicit if Path(explicit).exists() else None
    for candidate in FONT_CANDIDATES:
        if Path(candidate).exists():
            return candidate
    return None


def collect_chars(layout: dict[str, Any], extra: Iterable[str] = ()) -> list[int]:
    """布局里所有字符串 + 固定文案 + 可打印 ASCII，去重后按码点排序。"""
    chars: set[int] = set(range(0x20, 0x7F))

    def add_str(value: str) -> None:
        for ch in value:
            cp = ord(ch)
            if 0x20 <= cp <= 0xFFFF:
                chars.add(cp)

    def walk(node: Any) -> None:
        if isinstance(node, str):
            add_str(node)
        elif isinstance(node, dict):
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(layout)
    for text in extra:
        add_str(text)
    add_str(BASE_TEXT)
    return sorted(chars)


def _pack_bitmap(mask_bytes: bytes) -> bytes:
    """8-bit alpha → LVGL 4bpp（高半字节在前，行间连续打包）。"""
    out = bytearray()
    pending: int | None = None
    for value in mask_bytes:
        nibble = value >> 4
        if pending is None:
            pending = nibble
        else:
            out.append((pending << 4) | nibble)
            pending = None
    if pending is not None:
        out.append(pending << 4)
    return bytes(out)


def _section(label: bytes, payload: bytes) -> bytes:
    return struct.pack("<I", 8 + len(payload)) + label + payload


def build_binfont(chars: Iterable[int], size: int, font_path: str) -> bytes:
    """生成 LVGL binfont 字节流（无压缩、4bpp、无 kerning）。"""
    from PIL import ImageFont  # 延迟导入，没装 Pillow 也能启动服务

    codepoints = sorted({cp for cp in chars if 0x20 <= cp <= 0xFFFF})
    if not codepoints:
        raise ValueError("empty character set")

    font = ImageFont.truetype(font_path, size, index=0)
    ascent, descent_px = font.getmetrics()

    glyphs: list[tuple[int, int, int, int, int, bytes]] = []
    for cp in codepoints:
        ch = chr(cp)
        try:
            bbox = font.getbbox(ch)
            mask = font.getmask(ch, mode="L")
            adv = int(round(font.getlength(ch)))
        except Exception:
            bbox, mask, adv = (0, 0, 0, 0), None, size // 2
        left, top, right, bottom = bbox
        box_w, box_h = max(0, right - left), max(0, bottom - top)
        # LVGL 的 ofs_y 是“字形底部相对基线”的偏移：
        # 字形底边在基线上方为正，下方为负。Pillow 的 bbox 以行顶为原点。
        ofs_y = ascent - bottom
        if mask is None or box_w == 0 or box_h == 0:
            box_w = box_h = 0
            bitmap = b""
            ofs_x = 0
        else:
            bitmap = _pack_bitmap(bytes(mask))
            ofs_x = left
        # 8.4 定点：保留亚像素步进，英文高低宽窄不同的字母才不会挤在一起
        adv = max(0, min(65535, int(round(font.getlength(ch) * 16.0))))
        ofs_x = max(-128, min(127, ofs_x))
        ofs_y = max(-128, min(127, ofs_y))
        glyphs.append((adv, ofs_x, ofs_y, min(255, box_w), min(255, box_h),
                       bitmap))

    # glyph 0 是 notdef，保留 6 字节头、无位图
    glyph_records = [bytes(6)]
    for adv, ofs_x, ofs_y, box_w, box_h, bitmap in glyphs:
        glyph_records.append(
            bytes((adv >> 8, adv & 0xFF, ofs_x & 0xFF, ofs_y & 0xFF,
                   box_w, box_h)) + bitmap
        )

    offsets: list[int] = []
    pos = 8  # 跳过 glyf 段自身的 8 字节 label 头
    for record in glyph_records:
        offsets.append(pos)
        pos += len(record)

    min_y = min((g[2] for g in glyphs), default=0)
    max_y = max((g[2] + g[4] for g in glyphs), default=ascent)

    header = struct.pack(
        "<IHHHhHhHhhHHBBBBBBBBBBhH",
        4,              # version
        3,              # tables_count（<4 表示没有 kerning 段）
        size,           # font_size
        ascent,         # ascent
        -descent_px,    # descent
        ascent,         # typo_ascent
        -descent_px,    # typo_descent
        0,              # typo_line_gap
        min_y,          # min_y
        max_y,          # max_y
        size,           # default_advance_width
        0,              # kerning_scale
        1,              # index_to_loc_format: uint32
        1,              # glyph_id_format
        1,              # advance_width_format: 8.4 定点
        4,              # bits_per_pixel
        8,              # xy_bits
        8,              # wh_bits
        16,             # advance_width_bits：16 位存 8.4 步进
        0,              # compression_id: 无压缩
        0,              # subpixels_mode
        0,              # padding
        -2,             # underline_position
        2,              # underline_thickness
    )
    head = _section(b"head", header)

    range_start = codepoints[0]
    range_length = codepoints[-1] - range_start + 1
    if range_length > 0xFFFF:
        raise ValueError("character range too wide for LVGL cmap")
    # 一个 cmap：SPARSE_TINY，glyph id = 1 + 排序后的下标
    cmap_table = struct.pack(
        "<IIHHHBB",
        28,             # data_offset：8(label) + 4(count) + 16(table)
        range_start,
        range_length,
        1,              # glyph_id_start
        len(codepoints),
        3,              # LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
        0,
    )
    cmap_data = b"".join(
        struct.pack("<H", cp - range_start) for cp in codepoints
    )
    cmap = _section(b"cmap", struct.pack("<I", 1) + cmap_table + cmap_data)

    loca = _section(
        b"loca",
        struct.pack("<I", len(offsets)) + b"".join(
            struct.pack("<I", off) for off in offsets
        ),
    )
    glyf = _section(b"glyf", b"".join(glyph_records))

    return head + cmap + loca + glyf


def font_bytes(
    layout: dict[str, Any],
    font_path: str,
    size: int = 16,
    extra: Iterable[str] = (),
) -> tuple[bytes, list[int]]:
    chars = collect_chars(layout, extra)
    key = (font_path, size, _FORMAT_VERSION, hashlib.sha256(
        ",".join(str(cp) for cp in chars).encode()).hexdigest())
    with _LOCK:
        cached = _CACHE.get(key)
        if cached is not None:
            return cached, chars
    data = build_binfont(chars, size, font_path)
    with _LOCK:
        if len(_CACHE) > 12:
            _CACHE.clear()
        _CACHE[key] = data
    return data, chars
