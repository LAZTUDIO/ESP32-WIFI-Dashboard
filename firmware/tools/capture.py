#!/usr/bin/env python3
"""
从 ESP32 串口抓一帧画面，存成 PNG。

用法:
  capture.py                              抓当前画面到 /tmp/board.png
  capture.py --cmd "l 3"                  先跳到第 3 关再抓
  capture.py --cmd "g" --out /tmp/over.png
  capture.py --send "h"                   只发命令不抓帧（读 STATS/PERF）

依赖 IDF 自带的 pyserial:
  ~/.espressif/python_env/idf5.5_py3.12_env/bin/python tools/capture.py
"""
import argparse
import base64
import glob
import re
import sys
import time
import zlib

import serial

BEGIN = b"---SNAP-BEGIN"
END = b"---SNAP-END---"


def find_port():
    hits = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    if not hits:
        sys.exit("找不到串口设备（/dev/cu.usbmodem*）")
    return hits[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="/tmp/board.png")
    ap.add_argument("--cmd", default=None, help="抓帧前先发的命令，如 'l 3'")
    ap.add_argument("--send", default=None, help="只发命令，不抓帧")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--no-reset", action="store_true",
                    help="（已废弃）保留兼容，默认不复位")
    ap.add_argument("--reset", action="store_true",
                    help="抓帧前复位板子（复位后串口命令可能失效，慎用）")
    args = ap.parse_args()

    port = args.port or find_port()
    s = serial.Serial(args.port or port, args.baud, timeout=0.2)

    # 注意：不要用 DTR/RTS 复位！ESP32-S3 的 USB-Serial/JTAG 在 DTR 拉低后
    # 会认为主机断开，重新拉高也无法恢复 stdin，命令就发不进去了。
    # 抓帧只读当前画面，复位交给 --reset（调用 esptool）之外的手段。
    if args.reset:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
        time.sleep(1.2)

    s.setDTR(True)
    s.setRTS(False)
    time.sleep(0.3)
    s.reset_input_buffer()

    if args.send:
        s.write((args.send.strip() + "\n").encode())
        s.flush()
        t = time.time()
        buf = b""
        while time.time() - t < args.timeout:
            buf += s.read(4096)
        s.close()
        print(buf.decode("utf-8", "replace"))
        return

    if args.cmd:
        s.write((args.cmd.strip() + "\n").encode())
        s.flush()
        time.sleep(0.4)          # 等状态生效
        s.reset_input_buffer()   # 丢掉命令回显，避免混进 base64

    def png_ok(data):
        """逐块校验 CRC，能抓住串口丢字节造成的隐性损坏"""
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            return False
        i = 8
        while i + 12 <= len(data):
            ln = int.from_bytes(data[i:i + 4], "big")
            ctype = data[i + 4:i + 8]
            chunk = data[i + 8:i + 8 + ln]
            if len(chunk) != ln:
                return False
            crc = int.from_bytes(data[i + 8 + ln:i + 12 + ln], "big")
            if zlib.crc32(ctype + chunk) & 0xffffffff != crc:
                return False
            i += 12 + ln
            if ctype == b"IEND":
                return True
        return False

    result = None
    for attempt in range(3):
        s.write(b"s\n")
        s.flush()

        t = time.time()
        buf = b""
        while time.time() - t < args.timeout:
            buf += s.read(8192)
            if END in buf:
                break

        if BEGIN not in buf or END not in buf:
            print(f"第 {attempt + 1} 次没抓到帧，重试", file=sys.stderr)
            continue

        seg = buf[buf.index(BEGIN): buf.index(END)]
        header = seg.split(b"\n", 1)[0].decode("ascii", "replace")
        body = seg.split(b"\n", 1)[1]

        m = re.search(r"bytes=(\d+)", header)
        declared = int(m.group(1)) if m else None

        # 只保留"整行都是 base64"的行：日志行（含空格、冒号、括号）必须整行丢弃，
        # 否则把日志里的字母混进来会让 PNG 数据错位。
        clean = b"".join(
            ln.strip() for ln in body.split(b"\n")
            if re.fullmatch(rb"[A-Za-z0-9+/=]+", ln.strip())
        )
        png = base64.b64decode(clean + b"=" * (-len(clean) % 4))

        if declared and len(png) != declared:
            print(f"第 {attempt + 1} 次长度不符（{len(png)}/{declared}），重试",
                  file=sys.stderr)
            time.sleep(0.5)
            continue
        if not png_ok(png):
            print(f"第 {attempt + 1} 次 PNG 校验失败，重试", file=sys.stderr)
            time.sleep(0.5)
            continue

        result = (header, clean, png)
        break

    s.close()
    if result is None:
        print("三次抓帧都失败，检查串口是否被其他程序占用", file=sys.stderr)
        sys.exit(1)

    header, clean, png = result
    with open(args.out, "wb") as f:
        f.write(png)

    print(f"port={port}  header={header.strip()}")
    print(f"解码 {len(clean)} base64 -> {len(png)} 字节 PNG -> {args.out}")


if __name__ == "__main__":
    main()
