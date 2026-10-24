# ESP32-WIFI-Dashboard 0.3 协议（v1）

服务端与 Web 编辑器、ESP32 固件之间的最小契约。0.3 起服务端按设备管理多块屏：设备上报身份，服务端按 IP 接入并下发配置，布局/字体按设备隔离。

## 指标值格式

每个指标值统一为：

```json
{"v": 42.1, "a": true, "t": 1729671234.5}
```

- `v`：number / bool / string
- `a`：available，false 表示当前取不到（界面显示 `--`）
- `t`：采集时间（Unix 秒）

数值的标签、单位、精度、是否适合画曲线由 `shared/catalog/metrics.json` 提供。每个指标还带：

- `source`：`api`（设备轮询传统 API 得到）或 `server`（服务端聚合得到）
- `providers`：能提供该测点的 API 类型数组（`halogen` / `gufo` / `strata` / `lmstudio` / `ollama` / `openai`）；空数组表示只能由服务端快照提供。编辑器按屏幕实际探测到的类型筛选测点。
- 硬件测点（`host.*`）只有 `strata` / `panel` 的接口会直接返回；配置了主机监控地址（`host_agent`）的屏幕由设备轮询 host agent 获得，编辑器会单独列成「设备主机监控」分组。

## REST

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/health` | 服务状态、面板源状态 |
| GET | `/api/v1/bootstrap` | 指标目录 + 组件目录 + 画布 profile + 配色 |
| GET | `/api/v1/catalog` | 指标目录（每项带 `source`: `api` / `server`） |
| GET | `/api/v1/widgets` | 组件目录 |
| GET | `/api/v1/profiles` | 画布 profile |
| GET | `/api/v1/themes` | 配色主题与 token |
| GET | `/api/v1/snapshot` | 当前全部指标值 |
| GET | `/api/v1/sources` | 各数据源最近一次采集状态 |
| GET | `/api/v1/devices` | 设备列表（online/ip/profile/api/last_seen） |
| POST | `/api/v1/devices/connect` | `{ip, name?, api?, rot180?}` 探测并接入 |
| GET/PATCH/DELETE | `/api/v1/devices/{id}` | 读取 / 修改并下发 / 移除 |
| POST | `/api/v1/devices/{id}/push` | 重新下发配置 |
| POST | `/api/v1/devices/{id}/reboot` | 重启屏幕 |
| GET/PUT | `/api/v1/devices/{id}/layout` | 该屏布局；`resolved=1` 注入 `theme_tokens` |
| GET | `/api/v1/devices/{id}/font/meta` | 该屏 11/13/15 字体元信息 |
| GET | `/api/v1/devices/{id}/font?size=11` | 该屏指定字号 binfont |
| GET | `/api/v1/server-info` | 下发给设备的服务端地址（含转发地址） |
| GET/PUT | `/api/v1/forward` | 读取/修改 API 转发设置 |
| GET | `/api/v1/forward/{path}` | 把设备请求转发到上游 API；服务端注入密钥，不缓存 |
| GET/PUT | `/api/v1/layout` | 旧接口/模板布局（兼容） |

## WebSocket `/ws?device_id=<id>`

- 设备连接带 `device_id`（如 `esp32-a1b2c3`）；编辑器不带，作为 viewer。
- 连接后服务端先发 `{"type":"hello","v":1,"device_id":"..."}`，客户端发送订阅后才开始推送：

```json
{"type": "subscribe", "metrics": ["llm.decode_tps", "host.gpu_temp_c"]}
```

`metrics` 省略或为空表示订阅全部。随后服务端每 1 秒推送一次快照：

```json
{"type": "snapshot", "v": 1, "ts": 1729671234.5, "values": {"llm.decode_tps": {"v": 42.1, "a": true, "t": 1729671234.4}}}
```

- 订阅列表只影响该连接；固件只订阅布局里用到的指标。
- 保存某屏布局后，服务端向该设备 + 所有 viewer 发送
  `{"type":"layout_changed","device_id":"esp32-a1b2c3","profile":"320x480"}`；
  其他设备不会收到。设备重新拉取该设备的 `resolved=1` 布局并重载 UI，无需重启。
- 设备在 WS 连接建立时以及 API 类型/连接状态变化时上报：

```json
{"type": "status", "provider": "lmstudio", "api_ok": true, "api_configured": true}
```

  服务端把它写入设备 `runtime`，并向该设备与所有 viewer 广播
  `{"type":"device_status","device_id":"esp32-a1b2c3","runtime":{"provider":"lmstudio","api_ok":true,"api_configured":true,"ts":1729671234.5}}`。
  `provider` 为机器键（`halogen` / `gufo` / `strata` / `lmstudio` / `ollama` / `openai` / `panel` / `unknown`），编辑器据此筛选测点。
- 服务端回复 `{"type":"pong"}`。
- 连接断开后客户端每 2 秒重连，重连后需要重新订阅。

## 设备管理（0.3）

设备联网后在 **未连上服务端/断线恢复** 时开放管理 HTTP（:80，仅局域网）；WS 连上后自动停止以释放内部 RAM，配置改由 WS 下发。断线 10 秒后自动重新开放，便于按 IP 重新接入。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/info` | `{id,name,profile,fw,ip,ssid,configured,server_connected}` |
| POST | `/api/v1/config` | 首次接入/恢复时下发配置（见下） |
| POST | `/api/v1/reboot` | 重启设备（HTTP 兜底） |

在线时通过 WS 下发同样结构的配置：

```json
{"type": "config", "config": {"dashboard_url": "...", "device_id": "...", "name": "主屏", "profile": "320x480", "rot180": false, "api": {"url": "...", "key": "", "provider": "auto", "host_agent": ""}}}
{"type": "reboot"}
```

HTTP `/api/v1/config` body：

```json
{
  "dashboard_url": "http://192.168.1.116:8080",
  "device_id": "esp32-a1b2c3",
  "name": "主屏",
  "profile": "320x480",
  "rot180": false,
  "api": {"url": "http://192.168.1.100:8000", "key": "", "provider": "auto", "host_agent": ""}
}
```

- 设备保存到 NVS；API/名称/服务端地址立即生效，WS 重连并重拉布局；`rot180` 变更会重建设备 UI。
- 若屏幕 API 地址指向 `http://<服务端>:8080/api/v1/forward`，设备仍按自身逻辑探测/解析，服务端只做反向代理并注入 `Authorization: Bearer <key>`；转发设置保存在服务端 `server/data/forward.json`，未启用时该路径返回 503。
- 服务端在设备 WS 上线时会补发一次 `config`，离线期间改的配置自动同步；设备会比较后只应用变化。
- 设备 ID 由 MAC 派生（`esp32-xxxxxx`），服务端只登记，不能改。
- 管理接口无鉴权，仅限局域网；AP 配网时让位给 portal。
- 服务端记录在线状态来自 WS 连接；设备重新联网后 WS 对端 IP 会更新服务端记录。

## 布局 JSON

```json
{
  "v": 1,
  "profile": "320x480",
  "theme": "classic",
  "widgets": [
    {
      "id": "w-dec",
      "type": "kpi",
      "x": 0, "y": 2, "w": 6, "h": 3,
      "bindings": {"value": "llm.decode_tps"},
      "options": {"title": "解码速度"}
    }
  ]
}
```

- `x/y/w/h` 都是网格单位，范围由 profile 的 `cols/rows` 决定。
- `bindings` 的 key 由组件目录定义；value 是在指标目录里存在的指标 ID，类型必须匹配。
- 通用 `options.show_title`（默认 true）：为 false 时设备不渲染标题栏，服务端允许最小高度减 1 格；编辑器的边框拖动只是编辑辅助，不占布局。
- 服务端保存前会校验：组件类型、尺寸下限、越界、重叠、绑定指标类型、主题 id。
- 0.3 起布局按设备保存为 `server/data/layouts/<device_id>.json`；`<profile>.json` 与 preset 只作为新屏的初始模板。主题不参与几何计算。
- `theme` 只能是 `shared/catalog/themes.json` 里的 id；缺省视为 `classic`。
- 设备请求 `/api/v1/devices/{id}/layout?resolved=1` 时，服务端会写入 `theme_tokens`（见下）并把指标标签/单位/精度补全。

## 多页布局（v2）

```json
{
  "v": 2,
  "profile": "320x480",
  "theme": "classic",
  "page_animation": "slide",
  "pages": [
    {"name": "概览", "widgets": []},
    {"name": "服务", "widgets": []}
  ]
}
```

- `pages` 1..4 页；每页组件 ≤40，总组件 ≤20（设备内部 RAM 实测上限）；页面内不得重叠，组件 id 全局唯一。
- `page_animation`：`slide` / `fade` / `stack` / `zoom` / `flip` / `rotate` / `none`。
- v1 布局仍兼容；编辑器加载 v1 后按单页 v2 处理，保存时升级为 v2。

## 指标列表组件（metric_list）

`options.rows` 是行数组，每行：

```json
{"label": "在线", "binding": "llm.online", "kind": "status", "format": ""}
```

- `binding` 必须存在于指标目录。
- `kind`：`auto` / `status` / `text` / `number`；`number` 只能绑定数值指标。
- `format`：可选格式后缀，支持 `{v}` 占位。
- 行数 1..8，由组件目录定义。
- 该组件没有固定 `bindings`，服务端逐行校验。

## 配色主题（0.2.3，已实现）

配色只在 Web 编辑器里选择，设备不保存主题。主题目录：

```json
{
  "version": 1,
  "themes": [
    {
      "id": "classic",
      "label": "经典 · 石墨蓝",
      "tokens": {
        "bg": "#080B10", "surface": "#121822", "surface_2": "#18202C",
        "border": "#263242", "text": "#EDF2F8", "text_dim": "#8C9BAE",
        "text_mut": "#5A6B80", "accent": "#4D8DFF", "accent_2": "#8B5CF6",
        "cyan": "#22D3EE", "ok": "#34D399", "warn": "#FB923C",
        "err": "#F87171", "pink": "#F472B6", "radius": 12
      }
    }
  ]
}
```

- `GET /api/v1/themes` 返回同一份目录；`GET /api/v1/bootstrap` 的 `themes` 字段也包含它。
- `GET /api/v1/layout?profile=...&resolved=1` 会注入：

```json
{"theme": "strix", "theme_tokens": {"bg": "#050608", "surface": "#0C0F14", "...": "..."}}
```

- 固件 `ui_dynamic_build()` 解析 `theme_tokens` 并调用 `ui_theme_set()`；缺字段回退经典配色。
- 颜色为 `#RRGGBB`，`radius` 为像素整数（卡片圆角）。

## 字体子集（0.2.2，已实现）

服务端按布局实际显示的文字生成 LVGL binfont 子集，设备从 SPIFFS 按需加载，解决任意中文标签缺字。字号与 Web 设计一致，按 11 / 13 / 15 px 三档分别生成：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/devices/{id}/font/meta` | 该屏 `{sizes:[{size,sha256,chars,bytes}], sha256, chars, size, font}` |
| GET | `/api/v1/devices/{id}/font?size=11` | 该屏指定字号 binfont（4bpp、无压缩），带 `X-Font-SHA256` |
| GET | `/api/v1/font/meta?profile=320x480` | 旧接口/模板；顶层字段是 16px 兼容值 |
| GET | `/api/v1/font?profile=320x480&size=11` | 旧接口/模板字体；`size` 缺省 16 |

- 字符集 = 布局 JSON 里所有字符串 + 组件内置文案 + 可打印 ASCII，去重排序。
- 服务端需要 Pillow 和系统 CJK 字体；`config.json` 的 `font_path` 可显式指定，留空自动探测。
- 设备按字号逐个比对 SHA-256，写入 `/spiffs/font_11_a.bin` 等双槽，用
  `lv_binfont_create_ex()` 一次性把位图读进 RAM（每档约 8–20KB，合计约 45KB，走 PSRAM），避免渲染路径再读文件。
- 设备使用设备级接口：`/api/v1/devices/{id}/font/meta`、`/api/v1/devices/{id}/font?size=11`；
  `/api/v1/font*` 保留给旧固件/模板。
- 字体会在 `layout_changed` 后随布局一起热更新；失败则保留内置 498 字子集。
- layout / theme token / font 已全部通过同一份 resolved 布局下发；图片等二进制资源打包仍待后续里程碑。

## 版本

- 指标消息协议版本：`v = 1`
- 布局版本：`v = 1`（旧单页）/ `v = 2`（多页）
- 指标/组件/配色目录版本：`version = 1`
- 新增字段保持向后兼容；删除或改变语义时提升版本。
