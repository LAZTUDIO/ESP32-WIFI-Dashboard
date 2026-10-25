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

数值的标签、单位、精度、是否适合画曲线由 `shared/catalog/metrics.json` 提供（v2）。每个指标还带：

- `source`：`api`（设备轮询传统 API 得到）或 `server`（服务端聚合得到）
- `api`：该测点在引擎 HTTP API 里的原始遥测出处，形如「端点 + JSON 路径」或 Prometheus 指标名。**每个 `source=api` 的测点必须有这一项**，目录只收引擎 API 实测能交付的字段；编辑器下拉框悬停即可看到出处。
- `providers`：能提供该测点的 API 类型数组（`halogen` / `gufo` / `strata` / `lmstudio` / `ollama` / `openai` / `panel`）；空数组表示只能由服务端快照提供。编辑器按屏幕实际探测到的类型筛选测点。
- 硬件测点（`host.*`）只有 `strata` / `panel` 的接口会直接返回（`host_agent` 同理）；`halogen` / `gufo` / `lmstudio` / `ollama` 的引擎 API 完全没有 CPU / 内存 / 磁盘 / GPU 字段，这些测点在它们的屏幕上恒为不可用，界面显示 `--`。编辑器会单独列成「设备主机监控」分组。
- 只能由第三方聚合面板（`panel`，halogen-panel 8800 `/api/status`）拿到、任何引擎 API 都没有的点位（NPU / TTS / Embedding / 磁盘 / SSD / 局域网 IP / 服务名 / CPU 频率 / GPU pstate 与 sclk）已整体移出目录，不再作为可选测点。

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
| GET/PUT | `/api/v1/devices/{id}/layout` | 该屏布局；`resolved=1` 注入 `theme_tokens`，`profile=` 指定投影方向 |
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
- 带 `device_id` 的设备连接**只下发 `source=server` 的指标**；`source=api` 的 LLM/主机指标由设备自己直连或走服务端转发采集，避免服务端快照里的 `a:false` 每秒覆盖设备本地值。
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
- 网格：竖屏 `12×18`、横屏 `18×12`，`gap=8`、`padding=4`，两个方向的单元格都是 ≈18.7px 正方形。
- 组件尺寸从 `widgets.json` 的 `size_options` 里选，横竖屏尺寸一致；编辑器可拖动位置，也可从右下角或属性面板调整尺寸，拖拽会吸附到合法尺寸。
- `bindings` 的 key 由组件目录定义；value 是在指标目录里存在的指标 ID，类型必须匹配。
- 通用 `options.show_title`（默认 true）：为 false 时设备不渲染标题栏；它不再改变组件尺寸。
- 服务端保存前会校验：组件类型、尺寸必须在 `size_options` 内、越界、重叠、绑定指标类型、主题 id，并要求整页在两个方向都能排下。
- 0.3 起布局按设备保存为 `server/data/layouts/<device_id>.json`；`<profile>.json` 与 preset 只作为新屏的初始模板。主题不参与几何计算。
- `theme` 只能是 `shared/catalog/themes.json` 里的 id 或 aliases；缺省为 `neon-cyber`。
- 设备请求 `/api/v1/devices/{id}/layout?resolved=1` 时，服务端会写入 `theme_tokens`、`theme_skin`（见下）并把指标标签/单位/精度补全。
- 旧布局（任意尺寸/旧主题/多页）在启动迁移时自动归一化并备份，多页会按优先级压缩成单页；也可以用 `python -m app.core.migrate --dry-run/--apply` 手动检查。

允许尺寸（两个方向通用，单位：格）：

### 基础组件

| 类型 | 可选 w×h |
|---|---|
| status_pill | 12×2、12×3 |
| kpi | 4×3、6×3、6×4、8×4、12×3、12×4、12×6 |
| sparkline | 4×3、6×3、6×4、6×6、8×4、8×6、12×3、12×4、12×6 |
| server_card | 6×4、6×6、8×4、8×6、12×4、12×6 |
| desktop_pet | 4×4、6×4、6×6、8×6、8×8 |
| metric_list | 4×3、6×4、6×6、8×4、8×6、12×4、12×6 |

### 小组件 2.0（generation 2）

| 类型 | 可选 w×h |
|---|---|
| gauge_ring | 2×2、3×3、4×3、4×4、5×5、6×4、6×6 |
| led_dot | 2×2、3×3、4×4 |
| led_matrix | 4×4、5×5、6×4、6×6、8×4、8×6、12×4、12×6 |
| bar_strip | 4×2、6×2、8×2、12×2、4×3、6×3、8×3、12×3、12×4 |
| vbar_gauge | 2×4、3×4、2×6、3×6、4×6、4×8 |
| multi_bars | 4×4、6×3、6×4、4×6、6×6、8×4、8×6、12×4、12×6 |
| dual_line | 4×4、6×4、6×6、8×4、8×6、12×4、12×6 |
| big_stat | 4×4、6×4、6×6、8×4、8×6、12×4、12×6 |
| spark_row | 4×2、6×2、8×2、12×2、4×3、6×3、8×3、12×3、6×4 |
| text_badge | 2×4、4×2、6×2、8×2、12×2、4×3、6×3、8×3、12×3、2×6 |

小组件共同遵守的渲染约定（固件 `ui/src/ui_mini.c` 与 Web 编辑器同规则）：

- **标题栏自动省略**：卡片像素高度 < 60px（等价于 2 行高，如 12×2、2×2）时不画标题栏，
  空间全部留给图形；组件内部若需要标题会在图形内自带一行。
- **行内标签不重复**：卡片带标题栏时（高度 ≥ 60px），bar_strip 的行内标签显示
  **指标名**而不是组件名，避免同一个标题出现两次；2 行内联形态没有标题栏，仍显示组件名。
- **数值缩放**：优先用 `options.max_value`；未给时 `%` 与 `°C` 按 100 计；其余按
  `v/(v+1)` 软缩放，保证任何量级都能落在 0–1 内不溢出。
- **阈值着色**：`warn_above` / `danger_above` 为 0 表示关闭；越线时数值与填充同时转到
  `warn` / `err` 色，bar_strip 与 vbar_gauge 还会在轨道上画出阈值刻线。
- **行式组件**（led_matrix / multi_bars / spark_row）：数据来自 `options.rows`，每行
  `{label, binding, kind, format}`，`kind` 取 `status`（布尔）/`number`/`text`/`auto`；
  行数上限见 `widgets.json` 的 `rows.max`。
- **可空绑定**：`led_dot` 的 `subtitle`、`dual_line` 的 `value2` 建议填上，缺省时
  主指标照常显示，副信息位留空而不是报错。

图鉴与示例：

- `docs/design/widget-gallery-v2.html`：10 类 × 全部尺寸 × 四主题的实测图鉴（预览由
  `server/web/app.js` 真实渲染后序列化，`node docs/design/build_widget_gallery.mjs` 重新生成）。
- `examples/widgets-showcase-320x480.json`：一屏放下全部 10 类小组件的示例布局，
  含横屏 `alt` 坐标。想直接看效果：拷到 `server/data/layouts/<device_id>.json`
  （或作为新屏模板的 `320x480.json`），在编辑器点「重载」即可。

## 单页布局（v2）

```json
{
  "v": 2,
  "profile": "320x480",
  "theme": "neon-cyber",
  "pages": [
    {"name": "主页", "widgets": []}
  ]
}
```

- `pages` 固定 1 页；总组件 ≤20（设备内部 RAM 实测上限），组件不得重叠，id 全局唯一。
- 不再有 `page_animation`、翻页或滑动；省下的刷新预算用于组件局部动画与装饰。
- v1 / 多页 v2 布局仍兼容；编辑器加载后合并为单页，保存时升级为单页 v2。
- 放不下时按优先级保留：状态条 → 前 2 个 KPI → 服务器卡 → 桌宠 → 其余 KPI → 曲线 → 指标列表。

## 横竖屏：一份组件数据

同一块屏只保存一份组件列表；`profile` 是布局的主方向，组件可选 `alt` 记录另一方向的位置：

```json
{
  "id": "w-dec", "type": "kpi", "x": 0, "y": 2, "w": 6, "h": 3,
  "alt": {"x": 0, "y": 2},
  "bindings": {"value": "llm.decode_tps"},
  "options": {"title": "解码速度"}
}
```

- 组件类型/绑定/选项/尺寸只有一份；切换方向只改 `x/y`（主方向）或写入 `alt` 的 `x/y`（另一方向），不复制组件。
- `GET /api/v1/devices/{id}/layout?profile=480x320&resolved=1` 会按目标方向投影：有 `alt` 用 `alt`，没有则按像素比例映射位置并 first-fit；尺寸保持固定值，不会缩放。
- 保存时校验整页在两个方向都能放下；放不下请减少组件或拆页。
- 投影后的 resolved 布局带 `canvas`：`{profile,width,height,cols,rows,gap,padding}`，固件不再内置 320×480 常量。
- 设备配置的 `profile` 决定物理旋转：`320x480` → 0°/180°，`480x320` → 90°/270°；`rot180` 在基础方向上再翻 180°。

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

## 配色主题（v2，四套皮肤）

配色和组件皮肤只在 Web 编辑器里选择，设备不保存主题。主题目录：

```json
{
  "version": 2,
  "default": "neon-cyber",
  "aliases": {"classic": "neon-cyber", "strix": "vital-pulse", "tuf": "vintage-gauge"},
  "themes": [
    {
      "id": "neon-cyber",
      "label": "霓虹赛博",
      "tokens": {
        "bg": "#070B14", "surface": "#0B1424", "surface_2": "#101B30",
        "border": "#1B4A5A", "text": "#C9E8F2", "text_dim": "#5F8296",
        "text_mut": "#3E5A6B", "accent": "#00E5FF", "accent_2": "#FF3D8B",
        "cyan": "#00E5FF", "ok": "#9DFF00", "warn": "#FFB300",
        "err": "#FF5347", "pink": "#FF78A0", "radius": 4,
        "skin": "neon-cyber", "surface_grad_from": "#101B30",
        "surface_grad_to": "#090F1C", "glow": "#00E5FF", "glow_opa": 70,
        "border_width": 1, "corner_len": 8, "font_style": "mono",
        "tracking": 1, "deco": "corners", "pet_variant": "cyber-cat"
      }
    }
  ]
}
```

四套主题：`neon-cyber`（霓虹赛博）、`minimal-air`（极简流光）、`vintage-gauge`（复古机械）、`vital-pulse`（律动健身）。`skin` 决定固件组件皮肤，`surface_grad_from/to`、`glow`、`border_width`、`corner_len`、`font_style`、`tracking`、`deco`、`pet_variant` 为皮肤扩展字段；旧固件忽略未知字段。`aliases` 用于兼容旧布局里的 `classic/strix/tuf`。

- `GET /api/v1/themes` 返回 `themes/default/aliases`；`GET /api/v1/bootstrap` 的 `themes` 字段相同。
- `GET /api/v1/devices/{id}/layout?resolved=1` 会注入：

```json
{"theme": "vital-pulse", "theme_skin": "vital-pulse",
 "theme_tokens": {"bg": "#0A0C13", "surface": "#1B2033", "...": "..."}}
```

- 固件 `ui_dynamic_build()` 解析 `theme_tokens`，按 `skin` 渲染四套组件皮肤；缺字段回退霓虹赛博。
- 颜色为 `#RRGGBB`，`radius` 为像素整数（卡片圆角）。
- 固件面板为 ST7796 + BGR + `RGB565_SWAPPED`，实际固件里会把 R/B 通道换回，PC 模拟器不做该转换。

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
- 布局版本：`v = 1`（旧单页）/ `v = 2`（单页，`pages` 数组固定 1 项）
- 指标/组件/配色目录版本：`version = 1`
- 新增字段保持向后兼容；删除或改变语义时提升版本。
