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
| GET | `/api/v1/bootstrap` | 指标目录 + 组件目录 + 画布 profile |
| GET | `/api/v1/catalog` | 指标目录 |
| GET | `/api/v1/widgets` | 组件目录 |
| GET | `/api/v1/profiles` | 画布 profile |
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
- 服务端保存前会校验：组件类型、尺寸下限、越界、重叠、绑定指标类型。
- 布局按 `profile` 分开保存，主题不参与几何计算。

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

- `pages` 1..4 页；每页组件 ≤40，总组件 ≤64；页面内不得重叠，组件 id 全局唯一。
- `page_animation`：`slide` / `fade` / `none`。
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

## 配置包（规划，0.2.2）

`layout.json + theme.json + fonts/*.bin + assets/*` 打包，带 manifest 和 SHA-256；通过局域网推送到设备 LittleFS，校验后重载界面。配置下发与固件 OTA 分开：前者用于布局/主题/字体，后者只用于固件升级。

## 版本

- 指标消息协议版本：`v = 1`
- 布局版本：`v = 1`（旧单页）/ `v = 2`（多页）
- 指标/组件目录版本：`version = 1`
- 新增字段保持向后兼容；删除或改变语义时提升版本。
