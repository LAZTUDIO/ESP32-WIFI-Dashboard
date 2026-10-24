# ESP32-WIFI-Dashboard Server（0.3 多屏管理）

专用采集服务：把面板、系统、GPU 等多源数据统一成一套指标，通过 WebSocket 推给 Web 编辑器和 ESP32 屏；同时管理多块屏的配置与独立布局。服务端与推理 API 同机时，还可作为设备请求的转发端口，把上游地址和密钥收在服务端。

## 运行

```bash
cd server
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
copy config.example.json config.json
# 编辑 config.json 里的 panel_url
.venv\Scripts\python -m uvicorn app.main:app --host 0.0.0.0 --port 8080
```

浏览器打开：

- 控制台（设备管理）：`http://127.0.0.1:8080/console/`
- 布局编辑器：`http://127.0.0.1:8080/editor/`
- 手机/局域网：把 `127.0.0.1` 换成本机局域网 IP

Windows 第一次从手机访问时，防火墙可能会弹窗，需要允许专用网络。

### 图形启动器（Windows / macOS）

不想敲命令时，双击 `启动服务.bat`（Windows）或 `启动服务.command`（macOS），在窗口里点「启动 / 停止 / 重启」，日志实时显示；服务运行后可直接「打开控制台」。

- 首次运行若没有 `.venv` 或依赖不全，会自动创建虚拟环境并安装 `requirements.txt`（需要联网）
- 服务在后台运行；关闭窗口时会询问是否停止
- 选择「保持运行」关闭后，下次打开启动器会自动接管该服务（PID 记录在 `server/.launcher-server.pid`），仍可正常停止/重启
- 若端口被其他程序占用，状态显示「外部进程」且不会去结束它；只有确认响应本服务 API 的实例才会被接管
- 服务端输出写入 `server/.launcher-server.log`，启动器自身异常写入 `.launcher-error.log`
- macOS 若双击提示没有权限，执行一次 `chmod +x 启动服务.command`

## 多屏接入

设备联网后屏幕显示 IP。服务端 Web 顶部「连接屏幕」输入该 IP：

1. 服务端 `GET http://<IP>/api/v1/info` 探测设备 ID/分辨率/固件版本
2. 设备登记到 `server/data/devices.json`（ID 由 MAC 派生，如 `esp32-a1b2c3`）
3. 服务端 `POST http://<IP>/api/v1/config` 下发服务端地址、API 设置、备注名、翻转
4. 设备回连 `ws://<server>/ws?device_id=...`，并拉取 `server/data/layouts/<device_id>.json` 与字体
5. 编辑器「屏幕」下拉切换目标屏；保存布局只通知该屏

- 一台服务端可接多块屏；每屏独立布局/配色/API，服务端指标（panel/system/GPU）共享
- 控制台 `/console/` 按卡片管理屏幕：搜索、添加、配置、重命名、删除、布局、重启、服务状态
- 控制台「服务设置」可启用 API 转发：填写上游地址与密钥后，屏幕 API 地址填 `http://<服务端IP>:8080/api/v1/forward` 即可；设备仍自行探测 API 类型，服务端只转发并注入密钥
- 设备卡片会显示屏幕上报的 API 类型与连接状态；编辑器据此动态筛选测点，并区分「服务端转发」与「专用服务端」
- 设备在线时管理 HTTP 会停止以释放内部 RAM；配置/重启改走 WS。设备离线时保存配置会标记 pending，上线后自动同步
- 设备断线 10 秒后会自动重新开放管理 HTTP，方便按 IP 重新接入
- 新屏首次登记会从 `data/layouts/<profile>.json` 或 `shared/presets/default-<profile>.json` 复制初始布局
- 配网页可选填服务端地址；留空则由控制台按 IP 接入。API Key 在控制台填写，不需要在手机上输入
- 管理接口仅限局域网，v1 未做鉴权
- WS 上线会用对端 IP 自动更新服务端记录的设备 IP；DHCP 变化后可在控制台重新输入 IP 接入

## 配置

`server/config.json` 是本地配置，已被 gitignore。字段见 `config.example.json`：

- `panel_url`：可选的面板数据源地址（私有部署），默认取 `/api/status`
- `panel_interval_s`：面板轮询间隔，默认 1 秒
- `enable_system`：采集服务端本机 CPU/内存，默认开启
- `enable_gpu`：采集服务端本机 NVIDIA GPU，默认关闭（面板已提供主机 GPU 时不需要）
- `public_url`：下发给设备的服务端地址；留空自动使用局域网 IP（从 `127.0.0.1` 打开控制台时会自动换成 LAN IP）
- `font_path`：CJK 字体文件路径，用于按布局文字生成 LVGL 字体子集；留空自动探测系统字体

本地数据：

- `server/data/devices.json`：设备注册表（ID、备注名、IP、API 设置、运行时 API 类型/连接状态、在线时间）
- `server/data/forward.json`：API 转发设置（启用、上游地址、密钥、类型、超时）；密钥只保存在服务端
- `server/data/layouts/<device_id>.json`：每块屏的独立布局
- `server/data/layouts/<profile>.json` 与 `shared/presets/default-<profile>.json`：新屏的初始布局模板

字体子集需要 Pillow（已列入 `requirements.txt`）。没有可用的 CJK 字体时，`/api/v1/font*` 返回 503，设备继续使用固件内置的 498 字子集。

字体按 Web 设计的 **11 / 13 / 15 px** 三档分别生成，设备各存一套并用于标题、说明、服务器卡和指标列表（S/M/L）。`/font` 不带 `size` 时返回旧的 16px 子集，供旧固件兼容。

## 数据契约

- 指标目录：`shared/catalog/metrics.json`
- 组件目录：`shared/catalog/widgets.json`
- 画布 profile：`shared/catalog/profiles.json`
- 配色主题：`shared/catalog/themes.json`（经典 / STRIX / TUF；编辑器下拉与设备共用）
- 布局 schema：`shared/schema/layout.schema.json`
- 协议说明：`shared/protocol.md`
- 默认布局：`shared/presets/default-*.json`

## API

| 路径 | 说明 |
| --- | --- |
| `/api/v1/bootstrap` | 编辑器一次拉取指标/组件/画布/配色 |
| `/api/v1/themes` | 配色主题列表与 token |
| `/api/v1/snapshot` | 当前全部指标值 |
| `/api/v1/sources` | 各数据源状态 |
| `GET /api/v1/devices` | 设备列表（在线状态、IP、布局、API 设置） |
| `POST /api/v1/devices/connect` | 按 `{ip, name?, api?, rot180?}` 接入；在线屏走 WS，未上线屏走 HTTP |
| `GET/PATCH/DELETE /api/v1/devices/{id}` | 读取/修改配置/移除设备；PATCH 返回 `via=ws/http/pending` |
| `POST /api/v1/devices/{id}/push` | 重新下发配置（在线 WS、离线 HTTP，失败为 pending） |
| `POST /api/v1/devices/{id}/reboot` | 重启设备（在线走 WS） |
| `GET/PUT /api/v1/devices/{id}/layout` | 每屏独立布局；`resolved=1` 注入指标元数据与主题 token |
| `GET /api/v1/devices/{id}/font/meta` | 该屏字体的 11/13/15 三档元信息 |
| `GET /api/v1/devices/{id}/font?size=11` | 该屏指定字号的 binfont |
| `/ws?device_id=...` | 设备 WS（带 device_id）；编辑器连 `/ws` 作为 viewer |
| `/api/v1/server-info` | 下发给设备的服务端地址（含转发地址） |
| `GET/PUT /api/v1/forward` | 读取/修改 API 转发设置（`enabled/url/key/provider/timeout_s`） |
| `GET /api/v1/forward/{path}` | 转发到上游 API；服务端注入 `Authorization: Bearer <key>`，不缓存 |
| `GET/PUT /api/v1/layout?profile=...` | 旧接口/模板布局（兼容） |
| `/api/v1/font/meta`、`/api/v1/font` | 旧接口/模板字体（兼容） |

`layout_changed` 事件带 `device_id`，只发给目标设备与编辑器 viewer；保存某屏布局不会影响其他屏。设备上线时服务端会补发一次 `config` 消息，离线期间改的配置会自动同步。`config` / `reboot` 消息仅在线时通过 WS 下发。

## 当前范围

「设备管理 → 每屏布局/字体 → WebSocket 指标 → Web 编辑器 / ESP32」链路已打通。服务端 Web 按 IP 接入设备，每块屏有独立布局、配色和 API 设置；保存布局只通知目标屏。

设备可以同时启用两路数据源：传统 API 由 ESP32 直连轮询（地址/密钥由服务端下发），专用 Server（本服务）通过 WebSocket 推送服务端指标。`shared/catalog/metrics.json` 的每个指标带 `source`（`api` / `server`）和 `providers`（可提供该测点的 API 类型）字段；设备通过 WS `status` 消息上报实际探测到的类型（halogen / gufo / strata / lmstudio / ollama / openai），编辑器据此动态筛选：

- 「专用服务端 · <分组>」只列服务端快照里当前有值的指标
- 「设备直连 · <类型>」或「服务端转发 · <类型>」只列该类型 `providers` 包含的 API 测点
- 「设备主机监控 · <分组>」在该屏配置了主机监控地址（`host_agent`）时出现，列出设备由 host agent 轮询得到的硬件测点
- 转发未启用、类型未识别、API 断开时在下拉里给出对应提示，不再把全部测点混在一起
