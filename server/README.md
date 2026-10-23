# llmdash-server（0.2 M0/M1）

专用采集服务：把 halogen-panel、系统、GPU 等多源数据统一成一套指标，通过 WebSocket 推给 Web 编辑器和后续的 ESP32 固件。

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

- 本机：`http://127.0.0.1:8080/editor/`
- 手机/局域网：`http://<本机局域网IP>:8080/editor/`

Windows 第一次从手机访问时，防火墙可能会弹窗，需要允许专用网络。

## 配置

`server/config.json` 是本地配置，已被 gitignore。字段见 `config.example.json`：

- `panel_url`：halogen-panel 地址，默认取 `/api/status`
- `panel_interval_s`：面板轮询间隔，默认 1 秒
- `enable_system`：采集服务端本机 CPU/内存，默认开启
- `enable_gpu`：采集服务端本机 NVIDIA GPU，默认关闭（面板已提供主机 GPU 时不需要）

## 数据契约

- 指标目录：`shared/catalog/metrics.json`
- 组件目录：`shared/catalog/widgets.json`
- 画布 profile：`shared/catalog/profiles.json`
- 布局 schema：`shared/schema/layout.schema.json`
- 协议说明：`shared/protocol.md`
- 默认布局：`shared/presets/default-*.json`

## API

| 路径 | 说明 |
| --- | --- |
| `/api/v1/bootstrap` | 编辑器一次拉取指标/组件/画布 |
| `/api/v1/snapshot` | 当前全部指标值 |
| `/api/v1/sources` | 各数据源状态 |
| `/api/v1/layout?profile=320x480` | 读取布局 |
| `PUT /api/v1/layout` | 保存布局（校验越界/重叠/绑定类型） |
| `/ws` | 1 秒推送一次指标快照 |

## 当前范围

M0/M1 只打通「面板 → 服务端 → WebSocket → Web 编辑器」链路，固件端组件引擎和 WebSocket 客户端在 M2 接入。现有固件直连 provider 保持不变。
