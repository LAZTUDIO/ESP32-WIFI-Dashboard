#!/usr/bin/env python3
"""ESP32-WIFI-Dashboard 服务启动器（Windows / macOS）。

双击 启动服务.bat 或 启动服务.command 即可打开窗口，启动/停止看板服务。
"""

from __future__ import annotations

import json
import os
import queue
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path

try:
    import tkinter as tk
    from tkinter import messagebox, scrolledtext, ttk
except ImportError:
    msg = (
        "缺少 tkinter。macOS 可执行：brew install python-tk\n"
        "Windows 请安装 python.org 的官方 Python（自带 tkinter）。"
    )
    try:
        from pathlib import Path as _Path

        _Path(__file__).resolve().parent.joinpath(".launcher-error.log").write_text(
            msg, encoding="utf-8"
        )
    except Exception:
        pass
    try:
        print(msg)
    except Exception:
        pass
    sys.exit(1)

SERVER_DIR = Path(__file__).resolve().parent
VENV_DIR = SERVER_DIR / ".venv"
LOG_FILE = SERVER_DIR / ".launcher-server.log"
PID_FILE = SERVER_DIR / ".launcher-server.pid"
REQUIREMENTS = SERVER_DIR / "requirements.txt"
CONFIG_FILE = SERVER_DIR / "config.json"
CONFIG_EXAMPLE = SERVER_DIR / "config.example.json"

IS_WIN = os.name == "nt"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8080
DEP_PROBE = "import fastapi, uvicorn, websockets, httpx, psutil, PIL"


def venv_python() -> Path:
    return VENV_DIR / ("Scripts/python.exe" if IS_WIN else "bin/python")


def base_python() -> str:
    exe = Path(sys.executable)
    if IS_WIN and exe.name.lower() == "pythonw.exe":
        alt = exe.with_name("python.exe")
        if alt.exists():
            return str(alt)
    return str(exe)


def child_env() -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    return env


def spawn_kwargs() -> dict[str, object]:
    if IS_WIN:
        return {"creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0)}
    return {"start_new_session": True}


def load_config() -> tuple[str, int]:
    if not CONFIG_FILE.exists() and CONFIG_EXAMPLE.exists():
        shutil.copyfile(CONFIG_EXAMPLE, CONFIG_FILE)
    host, port = DEFAULT_HOST, DEFAULT_PORT
    try:
        data = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        host = str(data.get("host") or host)
        port = int(data.get("port") or port)
    except (OSError, ValueError):
        pass
    return host, port


def port_open(port: int, host: str = "127.0.0.1", timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def write_pid_file(pid: int, port: int) -> None:
    try:
        PID_FILE.write_text(
            json.dumps({"pid": pid, "port": port, "started": int(time.time())}),
            encoding="utf-8",
        )
    except OSError:
        pass


def read_pid_file() -> dict | None:
    try:
        data = json.loads(PID_FILE.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def clear_pid_file() -> None:
    try:
        PID_FILE.unlink()
    except OSError:
        pass


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if IS_WIN:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        try:
            code = wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == 259  # STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def port_owner(port: int) -> int | None:
    try:
        if IS_WIN:
            cmd = [
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                f"(Get-NetTCPConnection -LocalPort {port} -State Listen "
                "-ErrorAction SilentlyContinue | Select-Object -First 1).OwningProcess",
            ]
            out = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=8,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            text = out.stdout.strip()
            return int(text) if text.isdigit() else None
        out = subprocess.run(
            ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
            capture_output=True,
            text=True,
            timeout=8,
        )
        pids = [int(x) for x in out.stdout.split() if x.isdigit()]
        return pids[0] if pids else None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def terminate_pid(pid: int, timeout: float = 5.0) -> bool:
    if not pid_alive(pid):
        return True
    if IS_WIN:
        try:
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/T", "/F"],
                capture_output=True,
                timeout=8,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except (OSError, subprocess.SubprocessError):
            pass
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return True
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not pid_alive(pid):
            return True
        time.sleep(0.1)
    if not IS_WIN:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        deadline = time.time() + 3
        while time.time() < deadline:
            if not pid_alive(pid):
                return True
            time.sleep(0.1)
    return not pid_alive(pid)


def looks_like_dashboard(port: int) -> bool:
    import urllib.request

    try:
        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/v1/sources", timeout=1.5
        ) as resp:
            return resp.status == 200
    except Exception:
        return False


class ServerController:
    """不依赖 GUI 的服务进程管理，可在任意线程调用。"""

    def __init__(self, emit=print):
        self.emit = emit
        self.proc: subprocess.Popen | None = None
        self.adopted_pid: int | None = None
        self.listener_pid: int | None = None
        self.port = DEFAULT_PORT

    def ensure_env(self) -> Path:
        py = venv_python()
        if not py.exists():
            self.emit(f"未找到虚拟环境，正在创建：{VENV_DIR}")
            self._run_streamed([base_python(), "-m", "venv", str(VENV_DIR)], "venv")
            py = venv_python()
            if not py.exists():
                raise RuntimeError("虚拟环境创建失败")
        probe = subprocess.run(
            [str(py), "-c", DEP_PROBE],
            cwd=SERVER_DIR,
            capture_output=True,
            **spawn_kwargs(),
        )
        if probe.returncode != 0:
            self.emit("缺少依赖，正在安装（首次会稍慢）…")
            self._run_streamed(
                [str(py), "-m", "pip", "install", "-r", str(REQUIREMENTS)], "pip"
            )
        return py

    def _run_streamed(self, cmd: list[str], label: str) -> None:
        proc = subprocess.Popen(
            cmd,
            cwd=SERVER_DIR,
            env=child_env(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            **spawn_kwargs(),
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                self.emit(f"[{label}] {line}")
        code = proc.wait()
        if code != 0:
            raise RuntimeError(f"{label} 执行失败，退出码 {code}")

    def start(self) -> tuple[int, str]:
        host, port = load_config()
        if port_open(port):
            raise RuntimeError(
                f"端口 {port} 已被占用。请先停止占用该端口的程序，或修改 config.json 的 port。"
            )
        try:
            LOG_FILE.write_text("", encoding="utf-8")
        except OSError:
            pass
        py = self.ensure_env()
        self.emit(f"启动服务：http://127.0.0.1:{port}/console/")
        log_handle = open(LOG_FILE, "w", encoding="utf-8")
        try:
            self.proc = subprocess.Popen(
                [
                    str(py),
                    "-m",
                    "uvicorn",
                    "app.main:app",
                    "--host",
                    host,
                    "--port",
                    str(port),
                ],
                cwd=SERVER_DIR,
                env=child_env(),
                stdout=log_handle,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                **spawn_kwargs(),
            )
        finally:
            log_handle.close()
        self._wait_ready(port)
        self.port = port
        if self.proc is not None:
            # Windows 的 venv python.exe 是转发壳，真正监听端口的是子进程
            owner = port_owner(port)
            self.listener_pid = owner if owner and pid_alive(owner) else self.proc.pid
            write_pid_file(self.listener_pid, port)
        return port, host

    def _wait_ready(self, port: int, timeout: float = 8.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                self.proc = None
                try:
                    tail = LOG_FILE.read_text(
                        encoding="utf-8", errors="replace"
                    )[-1200:]
                except OSError:
                    tail = ""
                raise RuntimeError(
                    "服务启动后立即退出，可能是端口被占用或配置有误。\n\n"
                    + tail.strip()
                )
            if port_open(port):
                return
            time.sleep(0.15)
        self.stop()
        raise RuntimeError(f"服务启动超时（{timeout:.0f} 秒内端口 {port} 未就绪）")

    def adopt(self, pid: int, port: int) -> None:
        """接管已在运行的服务进程（如启动器关闭后保留下来的实例）。"""
        self.proc = None
        self.adopted_pid = pid
        self.listener_pid = pid
        self.port = port
        write_pid_file(pid, port)

    def forget(self) -> None:
        self.proc = None
        self.adopted_pid = None
        self.listener_pid = None
        clear_pid_file()

    def is_running(self) -> bool:
        if self.proc is not None and self.proc.poll() is None:
            return True
        if self.listener_pid is not None and pid_alive(self.listener_pid):
            return True
        if self.adopted_pid is not None and pid_alive(self.adopted_pid):
            return True
        return False

    def stop(self, timeout: float = 5.0) -> None:
        proc = self.proc
        self.proc = None
        if proc is not None and proc.poll() is None:
            self.emit("正在停止服务…")
            if IS_WIN:
                try:
                    subprocess.run(
                        ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                        capture_output=True,
                        timeout=8,
                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                    )
                except (OSError, subprocess.SubprocessError):
                    pass
                try:
                    proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    proc.kill()
            else:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    self.emit("服务未在预期时间内退出，强制结束")
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    proc.wait(timeout=3)
        target = self.listener_pid
        self.listener_pid = None
        if target is None:
            target = self.adopted_pid
        self.adopted_pid = None
        if target is not None and pid_alive(target):
            if proc is None:
                self.emit("正在停止服务…")
            if not terminate_pid(target, timeout):
                self.emit(f"进程 {target} 未能结束")
        clear_pid_file()


class LauncherApp(tk.Tk):
    IDLE, PREPARING, RUNNING, STOPPING = "idle", "preparing", "running", "stopping"

    STATUS = {
        IDLE: ("● 已停止", "#8a8f98"),
        PREPARING: ("● 正在准备…", "#d08a00"),
        RUNNING: ("● 运行中", "#1f9d55"),
        STOPPING: ("● 正在停止…", "#d08a00"),
    }

    def __init__(self):
        super().__init__()
        self.state = self.IDLE
        self.port = DEFAULT_PORT
        self.host = DEFAULT_HOST
        self.external = False
        self.log_offset = 0
        self.tail_active = False
        self._adopt_attempts = 0
        self.events: queue.Queue = queue.Queue()

        self.controller = ServerController(self._log_from_thread)
        self._build_ui()
        self._set_state(self.IDLE)
        self._update_detail()

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._closing = False
        self._poll_job = self.after(150, self._poll)
        self._external_job = self.after(100, self._check_external)

    # ---------- UI ----------

    def _build_ui(self):
        self.title("ESP32 Dashboard 服务启动器")
        self.geometry("940x640")
        self.minsize(800, 500)

        log_font = ("Consolas", 9) if IS_WIN else ("Menlo", 11)
        ui_font = ("Microsoft YaHei UI", 10) if IS_WIN else ("PingFang SC", 12)

        style = ttk.Style(self)
        style.configure("TButton", font=ui_font, padding=(14, 6))

        header = ttk.Frame(self, padding=(16, 14, 16, 6))
        header.pack(fill="x")
        self.status_label = tk.Label(
            header, text="", font=(ui_font[0], 13, "bold"), anchor="w"
        )
        self.status_label.pack(side="left")
        self.detail_label = tk.Label(header, text="", font=ui_font, fg="#5b616b")
        self.detail_label.pack(side="right")

        buttons = ttk.Frame(self, padding=(16, 6, 16, 10))
        buttons.pack(fill="x")
        self.start_btn = ttk.Button(buttons, text="启动", command=self._start_clicked)
        self.stop_btn = ttk.Button(buttons, text="停止", command=self._stop_clicked)
        self.restart_btn = ttk.Button(
            buttons, text="重启", command=self._restart_clicked
        )
        self.editor_btn = ttk.Button(
            buttons, text="打开控制台", command=self._open_editor
        )
        clear_btn = ttk.Button(buttons, text="清空日志", command=self._clear_log)
        for btn in (
            self.start_btn,
            self.stop_btn,
            self.restart_btn,
            self.editor_btn,
        ):
            btn.pack(side="left", padx=(0, 8))
        clear_btn.pack(side="right")

        body = ttk.Frame(self, padding=(16, 0, 16, 8))
        body.pack(fill="both", expand=True)
        self.log_text = scrolledtext.ScrolledText(
            body,
            wrap="none",
            font=log_font,
            state="disabled",
            background="#11151c",
            foreground="#d7dce4",
            insertbackground="#d7dce4",
            borderwidth=0,
        )
        self.log_text.pack(fill="both", expand=True)

        self.hint_label = tk.Label(
            self,
            text="停止会结束 uvicorn 进程；关闭窗口时也会询问是否停止服务。",
            font=ui_font,
            fg="#8a8f98",
            anchor="w",
        )
        self.hint_label.pack(fill="x", padx=16, pady=(0, 10))

        self._append_text("就绪。点击“启动”运行看板服务。\n")

    def _refresh_buttons(self):
        running = self.state == self.RUNNING and not self.external
        self.start_btn.configure(
            state="normal" if self.state == self.IDLE and not self.external else "disabled"
        )
        self.stop_btn.configure(state="normal" if running else "disabled")
        self.restart_btn.configure(state="normal" if running else "disabled")
        self.editor_btn.configure(
            state="normal" if self.state == self.RUNNING or self.external else "disabled"
        )

    def _set_state(self, state: str):
        self.state = state
        text, color = self.STATUS[state]
        self.status_label.configure(text=text, foreground=color)
        self._refresh_buttons()

    def _update_detail(self):
        self.host, self.port = load_config()
        if self.external:
            suffix = f"http://127.0.0.1:{self.port}/console/（外部进程）"
        elif self.state == self.RUNNING:
            tag = "（已接管）" if self.controller.adopted_pid else ""
            suffix = f"http://127.0.0.1:{self.port}/console/{tag}"
        else:
            suffix = f"端口 {self.port}"
        self.detail_label.configure(text=suffix)

    # ---------- 日志 ----------

    def _log_from_thread(self, text: str):
        self.events.put(("log", text))

    def _append_text(self, text: str):
        if not text:
            return
        at_bottom = self.log_text.yview()[1] > 0.99
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text if text.endswith("\n") else text + "\n")
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 5000:
            self.log_text.delete("1.0", f"{lines - 4000}.0")
        self.log_text.configure(state="disabled")
        if at_bottom:
            self.log_text.see("end")

    def _append_stamped(self, text: str):
        self._append_text(f"[{time.strftime('%H:%M:%S')}] {text}")

    def _tail_server_log(self):
        try:
            size = LOG_FILE.stat().st_size
        except OSError:
            return
        if size < self.log_offset:
            self.log_offset = 0
        if size == self.log_offset:
            return
        try:
            with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as fh:
                fh.seek(self.log_offset)
                data = fh.read()
                self.log_offset = fh.tell()
        except OSError:
            return
        if data:
            self._append_text(data)

    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    # ---------- 事件 ----------

    def _start_clicked(self):
        if self.state != self.IDLE:
            return
        self._begin_start()

    def _begin_start(self):
        self._set_state(self.PREPARING)
        self._append_stamped("准备运行环境…")

        def work():
            try:
                port, host = self.controller.start()
                self.events.put(("started", (port, host)))
            except Exception as exc:  # noqa: BLE001
                self.events.put(("error", str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def _stop_clicked(self):
        if self.state != self.RUNNING or self.external:
            return
        self._set_state(self.STOPPING)

        def work():
            try:
                self.controller.stop()
            except Exception as exc:  # noqa: BLE001
                self.events.put(("log", f"停止时出错：{exc}"))
            self.events.put(("stopped", None))

        threading.Thread(target=work, daemon=True).start()

    def _restart_clicked(self):
        if self.state != self.RUNNING or self.external:
            return
        self._set_state(self.STOPPING)
        self._append_stamped("正在重启…")

        def work():
            try:
                self.controller.stop()
                port, host = self.controller.start()
                self.events.put(("started", (port, host)))
            except Exception as exc:  # noqa: BLE001
                self.events.put(("error", str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def _open_editor(self):
        _, port = load_config()
        webbrowser.open(f"http://127.0.0.1:{port}/console/")

    # ---------- 轮询 ----------

    def _poll(self):
        if self._closing:
            return
        try:
            self._poll_tick()
            self._poll_job = self.after(150, self._poll)
        except (tk.TclError, RuntimeError):
            return

    def _poll_tick(self):
        self._drain_events()
        if self.tail_active:
            self._tail_server_log()
        if self.state == self.RUNNING and not self.controller.is_running():
            self._tail_server_log()
            self.tail_active = False
            self.controller.forget()
            self._append_stamped("服务进程已退出")
            self._set_state(self.IDLE)
            self._update_detail()

    def _drain_events(self):
        for _ in range(300):
            try:
                kind, payload = self.events.get_nowait()
            except Exception:
                break
            if kind == "log":
                self._append_text(payload)
            elif kind == "started":
                self.port, self.host = payload
                self.external = False
                self.log_offset = 0
                self.tail_active = True
                self._set_state(self.RUNNING)
                self._update_detail()
                self._append_stamped(f"服务已启动：http://127.0.0.1:{self.port}/console/")
            elif kind == "stopped":
                self._tail_server_log()
                self.tail_active = False
                self._set_state(self.IDLE)
                self._append_stamped("服务已停止")
                self._update_detail()
            elif kind == "error":
                self.log_offset = 0
                self._tail_server_log()
                self.tail_active = False
                self._set_state(self.IDLE)
                self._update_detail()
                self._append_stamped(f"启动失败：{payload}")
                messagebox.showerror("启动失败", payload, parent=self)

    def _check_external(self):
        if self._closing:
            return
        try:
            self._check_external_tick()
            self._external_job = self.after(2000, self._check_external)
        except (tk.TclError, RuntimeError):
            return

    def _check_external_tick(self):
        if self.state != self.IDLE:
            return
        _, port = load_config()
        was_external = self.external
        self.external = port_open(port)
        if self.external:
            if self._adopt_attempts < 3:
                self._adopt_attempts += 1
                if self._try_adopt(port):
                    return
        else:
            self._adopt_attempts = 0
        if self.external != was_external:
            if self.external:
                self.status_label.configure(
                    text="● 运行中（外部进程）", foreground="#1f9d55"
                )
                self._append_stamped(
                    f"检测到端口 {port} 已有服务在运行（非本启动器启动）"
                )
            else:
                text, color = self.STATUS[self.IDLE]
                self.status_label.configure(text=text, foreground=color)
            self._update_detail()
            self._refresh_buttons()

    def _try_adopt(self, port: int) -> bool:
        info = read_pid_file()
        pid = 0
        from_pid_file = False
        try:
            if info and int(info.get("port", -1)) == port:
                pid = int(info.get("pid", 0))
                from_pid_file = True
        except (TypeError, ValueError):
            pid = 0
        owner = port_owner(port)
        verified = owner is None or owner == pid
        if from_pid_file and pid and pid_alive(pid) and verified:
            pass
        elif owner and pid_alive(owner) and looks_like_dashboard(port):
            pid = owner
            from_pid_file = False
        else:
            return False
        self.controller.adopt(pid, port)
        self.port = port
        self.external = False
        self._set_state(self.RUNNING)
        if from_pid_file:
            self.tail_active = True
            try:
                self.log_offset = LOG_FILE.stat().st_size
            except OSError:
                self.log_offset = 0
        self._append_stamped(f"已接管运行中的服务（PID {pid}）")
        self._update_detail()
        return True

    # ---------- 关闭 ----------

    def destroy(self):
        self._closing = True
        for job in (self._poll_job, self._external_job):
            if job:
                try:
                    self.after_cancel(job)
                except Exception:
                    pass
        super().destroy()

    def _on_close(self):
        if self.state == self.PREPARING:
            messagebox.showinfo("请稍候", "正在准备运行环境，完成后再关闭。", parent=self)
            return
        if self.controller.is_running():
            choice = messagebox.askyesnocancel(
                "退出",
                "服务仍在运行。\n\n是：停止服务并退出\n否：保持运行，仅关闭窗口（下次打开会自动接管）\n取消：返回",
                parent=self,
            )
            if choice is None:
                return
            if choice:
                self._append_stamped("停止服务…")
                self.update_idletasks()
                self.controller.stop()
        self.destroy()


def main() -> None:
    if IS_WIN:
        try:
            from ctypes import windll

            windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    app = LauncherApp()
    app.mainloop()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback

        detail = traceback.format_exc()
        try:
            (SERVER_DIR / ".launcher-error.log").write_text(detail, encoding="utf-8")
        except OSError:
            pass
        try:
            messagebox.showerror("启动器出错", detail[-2000:])
        except Exception:
            print(detail)
        raise
