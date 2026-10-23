# LLM Dashboard 0.2 协议（v1）

服务端与 Web 编辑器、ESP32 固件之间的最小契约。M0/M1 先实现 REST + WebSocket 快照；后续可在此基础上加增量、主题下发和命令通道。

## 指标值格式

每个指标值统一为：

```json
{"v": 42.1, "a": true, "t": 1729671234.5}
```

- `v`：number / bool / string
- `a`：available，false 表示当前取不到（界面显示 `--`）
- `t`：采集时间（Unix 秒）

数值的标签、单位、精度、是否适合画曲线由 `shared/catalog/metrics.json` 提供。

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
| GET | `/api/v1/layout?profile=320x480` | 读取布局（无保存记录时返回 preset） |
| PUT | `/api/v1/layout` | 保存布局（body 为布局 JSON） |
| GET | `/api/v1/sources` | 各数据源最近一次采集状态 |

## WebSocket `/ws`

连接后服务端先发 `{"type":"hello","v":1}`，客户端发送订阅后才开始推送：

```json
{"type": "subscribe", "metrics": ["llm.decode_tps", "host.gpu_temp_c"]}
```

`metrics` 省略或为空表示订阅全部。随后服务端每 1 秒推送一次快照：

```json
{"type": "snapshot", "v": 1, "ts": 1729671234.5, "values": {"llm.decode_tps": {"v": 42.1, "a": true, "t": 1729671234.4}}}
```

- 订阅列表只影响该连接；固件只订阅布局里用到的指标。
- 服务端保存布局后向所有连接广播 `{"type":"layout_changed","profile":"320x480"}`；设备应重新拉取 `resolved=1` 布局并重载 UI，无需重启。
- 服务端回复 `{"type":"pong"}`。
- 连接断开后客户端每 2 秒重连，重连后需要重新订阅。

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
- 布局按 `profile` 分开保存，主题不参与几何计算。
- `theme` 只能是 `shared/catalog/themes.json` 里的 id；缺省视为 `classic`。
- 设备请求 `resolved=1` 时，服务端会写入 `theme_tokens`（见下）并把指标标签/单位/精度补全。

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

服务端按布局实际显示的文字生成 LVGL binfont 子集，设备从 SPIFFS 按需加载，解决任意中文标签缺字：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/font/meta?profile=320x480` | `{sha256, chars, size, font}`；设备先比对 sha256 |
| GET | `/api/v1/font?profile=320x480` | 二进制 binfont（4bpp、无压缩），带 `X-Font-SHA256` |

- 字符集 = 布局 JSON 里所有字符串 + 组件内置文案 + 可打印 ASCII，去重排序。
- 服务端需要 Pillow 和系统 CJK 字体；`config.json` 的 `font_path` 可显式指定，留空自动探测。
- 设备下载后校验 SHA-256，写入 `/spiffs/font_a.bin` / `font_b.bin` 双槽，用
  `lv_binfont_create_ex()` 一次性把位图读进 RAM（约 20–30KB，走 PSRAM），避免渲染路径再读文件。
- 字体会在 `layout_changed` 后随布局一起热更新；失败则保留内置 498 字子集。
- layout / theme token / font 已全部通过同一份 resolved 布局下发；图片等二进制资源打包仍待后续里程碑。

## 版本

- 指标消息协议版本：`v = 1`
- 布局版本：`v = 1`（旧单页）/ `v = 2`（多页）
- 指标/组件/配色目录版本：`version = 1`
- 新增字段保持向后兼容；删除或改变语义时提升版本。
