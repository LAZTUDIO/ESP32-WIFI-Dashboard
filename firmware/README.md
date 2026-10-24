# firmware —— ESP-IDF 工程

ESP32-WIFI-Dashboard 的固件部分。项目介绍、配网流程、支持的服务类型见[根目录 README](../README.md)。

## 构建

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <串口> flash
```

- 依赖由 `main/idf_component.yml` 自动下载（LVGL 9.6、esp_lvgl_port 2.9、esp_lcd_touch）
- 分区表在 `partitions.csv`（4 MB 应用分区 + 1 MB storage），关键选项见 `sdkconfig.defaults`
- `main/Kconfig.projbuild` 可改成 headless 模式（不接屏，仅串口打印刷新率）

## 目录

| 路径 | 说明 |
| --- | --- |
| `main/display.c` | ST7796 + TCA9554 复位 + LEDC 背光；内部 DMA 双缓冲、运行时 0/90/180/270 旋转 |
| `main/board_touch.c` | FT6336 触摸 |
| `main/wifi_mgr.c` | STA 连接、自动重连、SoftAP |
| `main/portal.c` | 手机配网页 + DNS 劫持 |
| `main/app_config.c` | NVS 配置读写（Wi-Fi / 看板服务地址 / 180° 翻转） |
| `main/dashboard_client.c` | 动态看板：拉布局（含配色 token）/字体、SPIFFS 缓存、WS、原子换屏 |
| `main/debug_console.c` | 串口抓帧、页面切换、渲染与动画帧率基准 |
| `../ui/src/ui_dynamic.c` | 布局 JSON 解析、组件创建、配色 token 应用 |
| `../ui/src/ui_widgets.c` | 通用控件与运行时配色（`ui_theme`） |
| `components/` | 板载 ST7796 / FT6336 驱动 |
| `tools/capture.py` | 串口抓帧（PNG） |
| `tools/host_agent.py` | 主机功耗/温度/内存监控小服务，供服务端采集使用 |
| `docs/DEBUGGING.md` | 板级调试与踩坑记录 |

## 串口调试

`?` 帮助、`h` 状态、`s` 抓帧、`p <n>` 切页、`v <0-6>` 临时切翻页动画、
`b` 整屏耗时、`B` 渲染/传输拆分、`C` 空白屏基线、`a`/`A` 动画帧率、
`w <ssid>|<pass>` 配网、`c <url>|<key>` 配传统 API（`c -` 清空）、`o <url>` 配主机监控（`o -` 清空）、
`d <url>` 配看板服务（`d -` 清空）、`P` 进入配网、`m` 导出指标存储。

## 动态看板模式（0.2）

配网页可分别填写传统 API 与看板服务地址，或串口执行 `c` / `d`。有看板地址时，设备拉取布局（含 `theme_tokens`）并用 WebSocket 订阅指标；有传统 API 地址时，`metrics.c` 独立轮询 halogen / gufo / strata / LM Studio / Ollama，结果写入同一份 `dash_metric_store`。两路可同时开启，同 ID 以最后写入为准。

- 长按 BOOT（GPIO0）3 秒进入配网（APSTA），屏幕显示热点/密码/地址；串口 `P` 也可触发
- 服务端保存布局后广播 `layout_changed`，设备自动重新拉取并热更新，无需重启
- 布局 v2 多页触摸：`slide` 原生跟手（约 440–470ms、15fps）；`fade`/`stack`/`zoom`/`flip`/`rotate` 关闭原生滚动，松手按水平位移（>50px）判定，用 PSRAM 截图做不透明变换（fade 为交叉淡出），约 200–390ms；`none` 直切
- 总组件数服务端上限 20；25 组件时内部最大连续块仅 3.4KB，WebSocket 断线重建会失败
- 最近一次 resolved 布局缓存到 `/spiffs/layout.json`，服务端不可用时开机可先用缓存；拿到 IP 后会再拉一次
- WS 重连后自动重拉布局，内容哈希相同则跳过重建
- 热更新 ≤10 组件时原子换屏：新屏建好后再切换并释放旧屏，避免黑屏；更多组件退回先删后建
- 字体：服务端按布局文字生成 LVGL binfont（`/api/v1/font`），设备比对 SHA-256 后写入 SPIFFS 双槽
  （`font_a.bin` / `font_b.bin`），一次性把位图加载进 RAM（约 20–30KB，走 PSRAM）；
  布局更新会顺带更新字体，失败时回退内置 498 字子集（`ui/src/font_cjk_16.c`）
- 运行时字体增加了内部 RAM 碎片压力：WebSocket 任务栈改为 5KB、收发缓冲 2KB，否则 6KB 栈可能创建失败

## 性能相关

- 编译选项：`-O2`、CPU 240MHz、32KB 指令缓存（`sdkconfig.defaults`）
- 绘制缓冲：内部 SRAM + DMA，1/12 屏 ×2，避免 PSRAM bounce buffer 导致的 flush 卡死
- 实测（320×480）：整屏重绘 63–74 ms（渲染 55–73 ms + SPI 约 6 ms），64px 对象动画 35 fps，整屏纯色动画 30 fps；数据 apply 1.0–2.1 ms
