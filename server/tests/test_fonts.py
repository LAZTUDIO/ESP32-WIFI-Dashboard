from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from app.core.fonts import build_binfont, collect_chars, find_font_path  # noqa: E402

HEAD_FMT = "<IHHHhHhHhhHHBBBBBBBBBBhH"


def parse_sections(data: bytes) -> dict[bytes, tuple[int, int]]:
    result: dict[bytes, tuple[int, int]] = {}
    off = 0
    while off < len(data):
        length = struct.unpack_from("<I", data, off)[0]
        label = data[off + 4:off + 8]
        result[label] = (off, length)
        off += length
    assert off == len(data), "section lengths must cover the file exactly"
    return result


class FontSubsetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.font_path = find_font_path()
        if not cls.font_path:
            raise unittest.SkipTest("no CJK font on this machine")

    def test_collect_chars(self) -> None:
        layout = {
            "v": 2,
            "pages": [
                {"name": "概览", "widgets": [
                    {"options": {"title": "温度曲线", "unit": "℃"}},
                    {"options": {"rows": [{"label": "在线状态"}]}},
                ]},
            ],
        }
        chars = collect_chars(layout)
        cp = set(chars)
        self.assertIn(ord("温"), cp)
        self.assertIn(ord("℃"), cp)
        self.assertIn(ord("概"), cp)
        self.assertIn(ord("A"), cp)  # 可打印 ASCII 始终包含
        self.assertEqual(chars, sorted(cp))

    def test_binfont_structure(self) -> None:
        chars = collect_chars({"title": "温度曲线"})
        data = build_binfont(chars, 16, self.font_path)

        sections = parse_sections(data)
        for label in (b"head", b"cmap", b"loca", b"glyf"):
            self.assertIn(label, sections)
        self.assertEqual(struct.calcsize(HEAD_FMT), 40)

        head_off, head_len = sections[b"head"]
        self.assertEqual(head_len, 48)
        header = struct.unpack_from(HEAD_FMT, data, head_off + 8)
        self.assertEqual(header[0], 4)       # version
        self.assertEqual(header[2], 16)      # font size
        self.assertEqual(header[14], 1)      # advance_width_format: 8.4
        self.assertEqual(header[15], 4)      # bpp
        self.assertEqual(header[16], 8)      # xy_bits
        self.assertEqual(header[17], 8)      # wh_bits
        self.assertEqual(header[18], 16)     # advance_width_bits

        cmap_off, _ = sections[b"cmap"]
        self.assertEqual(struct.unpack_from("<I", data, cmap_off + 8)[0], 1)
        (data_off, range_start, range_length, glyph_id_start,
         entries, fmt_type, _) = struct.unpack_from(
            "<IIHHHBB", data, cmap_off + 12)
        self.assertEqual(fmt_type, 3)        # SPARSE_TINY
        self.assertEqual(glyph_id_start, 1)
        self.assertEqual(entries, len(chars))
        # cmap 的 unicode_list 是相对码点，且升序
        rel = struct.unpack_from(f"<{entries}H", data, cmap_off + data_off)
        self.assertEqual(list(rel), sorted(rel))
        self.assertEqual(range_start + rel[0], chars[0])
        self.assertEqual(range_start + rel[-1], chars[-1])
        self.assertLessEqual(range_length, 0xFFFF)

        loca_off, _ = sections[b"loca"]
        count = struct.unpack_from("<I", data, loca_off + 8)[0]
        self.assertEqual(count, len(chars) + 1)  # notdef + 每个字符
        offsets = [
            struct.unpack_from("<I", data, loca_off + 12 + 4 * i)[0]
            for i in range(count)
        ]
        self.assertEqual(offsets[0], 8)
        self.assertTrue(all(a <= b for a, b in zip(offsets, offsets[1:])))

        glyf_off, glyf_len = sections[b"glyf"]
        self.assertEqual(glyf_off + glyf_len, len(data))
        for i in range(count):
            nxt = offsets[i + 1] if i + 1 < count else glyf_len
            self.assertGreaterEqual(nxt - offsets[i], 6)
            box_w = data[glyf_off + offsets[i] + 4]
            box_h = data[glyf_off + offsets[i] + 5]
            expected = 6 + (box_w * box_h + 1) // 2
            self.assertEqual(nxt - offsets[i], expected, f"glyph {i}")


if __name__ == "__main__":
    unittest.main()
