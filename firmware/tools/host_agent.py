#!/usr/bin/env python3
"""
主机监控小服务 —— 给 ESP32 看板补充 halogen/gufo 等 API 拿不到的硬件指标。

看板每轮会请求 GET http://<host>:8732/stats，返回 JSON：
  power_w  gpu_temp_c  gpu_pct  gpu_name
  vram_used_mb  vram_total_mb  ram_used_mb  ram_total_mb
  uptime_s  model_uptime_s

用法（服务器上，只依赖标准库）：
  python3 host_agent.py                 # 监听 0.0.0.0:8732
  python3 host_agent.py --port 9000
  python3 host_agent.py --model-match llama-server   # 用它算模型运行时长

说明：
  - 有 NVIDIA 显卡时走 nvidia-smi，否则读 /sys/class/hwmon（Linux）
  - 无 GPU 的机器只返回内存、开机时长等基础字段
"""

import argparse
import json
import os
import platform
import re
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

START = time.time()


def run(cmd, timeout=2):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True,
                             timeout=timeout)
        return out.stdout if out.returncode == 0 else ""
    except Exception:
        return ""


def num(text):
    m = re.search(r"([-\d.]+)", text)
    return float(m.group(1)) if m else None


def nvidia():
    """返回 (power_w, temp_c, gpu_pct, used_mb, total_mb, name)"""
    q = run(["nvidia-smi",
             "--query-gpu=power.draw,temperature.gpu,utilization.gpu,"
             "memory.used,memory.total,name",
             "--format=csv,noheader,nounits"])
    if not q.strip():
        return None
    first = q.strip().splitlines()[0].split(",")
    if len(first) < 6:
        return None
    try:
        return (float(first[0]), float(first[1]), float(first[2]),
                float(first[3]), float(first[4]), first[5].strip())
    except ValueError:
        return None


def hwmon_temp():
    base = "/sys/class/hwmon"
    if not os.path.isdir(base):
        return None
    for d in sorted(os.listdir(base)):
        name_path = os.path.join(base, d, "name")
        try:
            with open(name_path) as f:
                name = f.read().strip().lower()
        except OSError:
            continue
        if name not in ("coretemp", "k10temp", "zenpower", "cpu_thermal"):
            continue
        try:
            with open(os.path.join(base, d, "temp1_input")) as f:
                return int(f.read().strip()) / 1000.0
        except OSError:
            continue
    return None


def mem_info():
    """返回 (used_mb, total_mb)"""
    system = platform.system()
    if system == "Linux":
        total = avail = None
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    k, v = line.split(":", 1)
                    kb = num(v)
                    if k == "MemTotal":
                        total = kb
                    elif k == "MemAvailable":
                        avail = kb
        except OSError:
            return None, None
        if not total or avail is None:
            return None, None
        used = total - avail
        return used / 1024.0, total / 1024.0

    if system == "Darwin":
        total = num(run(["sysctl", "-n", "hw.memsize"]))
        vm = run(["vm_stat"])
        page = num(vm.split("\n")[0]) if vm else 4096
        free = 0.0
        m = re.search(r"Pages free:\s+(\d+)", vm)
        if m:
            free = float(m.group(1))
        if total:
            return (total - free * page) / (1024 * 1024), total / (1024 * 1024)
    return None, None


def uptime_s():
    system = platform.system()
    if system == "Linux":
        try:
            with open("/proc/uptime") as f:
                return int(float(f.read().split()[0]))
        except OSError:
            return int(time.time() - START)
    if system == "Darwin":
        v = run(["sysctl", "-n", "kern.boottime"])
        m = re.search(r"sec = (\d+)", v)
        if m:
            return int(time.time()) - int(m.group(1))
    return int(time.time() - START)


def model_uptime(match):
    """按进程名/命令行匹配 token 引擎进程，返回其已运行秒数"""
    if not match:
        return None
    if platform.system() == "Linux":
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    cmd = f.read().replace(b"\0", b" ").decode("utf-8", "replace")
            except OSError:
                continue
            if match not in cmd or "host_agent" in cmd:
                continue
            try:
                with open(f"/proc/{pid}/stat") as f:
                    fields = f.read().split()
                start_ticks = float(fields[21])
                with open("/proc/uptime") as f:
                    up = float(f.read().split()[0])
                hz = os.sysconf("SC_CLK_TCK") or 100
                return int(up - start_ticks / hz)
            except (OSError, IndexError, ValueError):
                return None
    else:
        out = run(["ps", "-axo", "pid=,etimes=,command="])
        for line in out.splitlines():
            if match in line and "host_agent" not in line:
                parts = line.split(None, 2)
                if len(parts) >= 2 and parts[1].isdigit():
                    return int(parts[1])
    return None


def collect(model_match):
    data = {"uptime_s": uptime_s()}
    mu = model_uptime(model_match)
    if mu is not None:
        data["model_uptime_s"] = mu

    used, total = mem_info()
    if used is not None:
        data["ram_used_mb"] = round(used)
        data["ram_total_mb"] = round(total)

    gpu = nvidia()
    if gpu:
        power, temp, util, used_mb, total_mb, name = gpu
        data.update(power_w=power, gpu_temp_c=temp, gpu_pct=util,
                    vram_used_mb=round(used_mb),
                    vram_total_mb=round(total_mb), gpu_name=name)
    else:
        t = hwmon_temp()
        if t is not None:
            data["cpu_temp_c"] = round(t, 1)
    return data


class Handler(BaseHTTPRequestHandler):
    model_match = None

    def do_GET(self):
        if self.path.split("?")[0] not in ("/stats", "/"):
            self.send_error(404)
            return
        body = json.dumps(collect(self.model_match)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8732)
    ap.add_argument("--model-match", default="",
                    help="进程命令行关键字，用来估算模型运行时长，如 llama-server")
    args = ap.parse_args()

    Handler.model_match = args.model_match
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"host agent -> http://{args.host}:{args.port}/stats")
    srv.serve_forever()


if __name__ == "__main__":
    main()
