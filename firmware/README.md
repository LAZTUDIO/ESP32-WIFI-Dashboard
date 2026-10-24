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
| `main/portal.c` | 手机配网页（只填 Wi-Fi）+ DNS 劫持 |
| `main/device_api.c` | STA 管理 HTTP：`/api/v1/info`、`/config`、`/reboot` |
| `main/app_runtime.h` | 配置快照、热应用、服务端连接状态、网络信息浮层 |
| `main/app_config.c` | NVS 配置读写（Wi-Fi / 设备 ID / API / 服务端地址） |
| `main/dashboard_client.c` | 动态看板：按设备拉布局/字体、SPIFFS 缓存、WS、热重配 |
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

## 动态看板模式（0.3）

**配网只填 Wi-Fi。** 设备联网后显示 IP/设备 ID；电脑服务端 Web 按 IP 接入，通过设备管理 HTTP 下发服务端地址、传统 API、备注名和翻转设置。设备随后连 `ws://<server>/ws?device_id=...`，拉取该屏的布局和字体。传统 API 由设备直连轮询（地址/密钥由服务端下发），结果写入同一份 `dash_metric_store`。

- 长按 BOOT（GPIO0）3 秒进入配网（APSTA），屏幕显示热点/密码/地址；串口 `P` 也可触发
- 网络信息浮层：已连 Wi-Fi、等待服务端时显示 SSID/IP/设备 ID；WS 连上后自动收起
- 管理 HTTP（:80）只在未连服务端/断线恢复时运行；WS 连上后自动停止，释放约 13KB 内部 RAM；断线 10 秒后自动重新开放
- 在线配置（API/名称/翻转）由服务端通过 WS 的 `config` 消息下发；离线保存的配置会在上线时自动同步；`reboot` 也走 WS
- 设备通过 WS `status` 消息上报实际探测到的 API 类型与连接状态（`provider` / `api_ok` / `api_configured`），编辑器据此动态筛选测点；若 API 地址指向服务端转发，探测结果不变，只是请求经过服务端
- 配网页可选填服务端地址：填了设备直接连，留空则由电脑端按 IP 接入；API Key 只在电脑端填写
- 配置热更新：API/名称/翻转/服务端地址下发后立即应用，不重启；旋转会重建设备 UI
- 服务端保存某屏布局后只通知该设备，设备重新拉取并热更新
- 布局 v2 多页触摸：`slide` 原生跟手（约 440–470ms、15fps）；`fade`/`stack`/`zoom`/`flip`/`rotate` 关闭原生滚动，松手按水平位移（>50px）判定，用 PSRAM 截图做不透明变换（fade 为交叉淡出），约 200–390ms；`none` 直切
- 总组件数服务端上限 20；25 组件时内部最大连续块仅 3.4KB，WebSocket 断线重建会失败
- 最近一次 resolved 布局缓存到 `/spiffs/layout.json`，服务端不可用时开机可先用缓存；拿到 IP 后会再拉一次
- WS 重连后自动重拉布局，内容哈希相同则跳过重建
- 在线时内部 RAM 约 39KB；原子换屏阈值 32KB，原子构建失败会自动回退“先删后建”，保证布局更新
- 字体：服务端按 Web 设计生成 11/13/15 三档 LVGL binfont（`/api/v1/devices/{id}/font?size=`），设备逐档比对 SHA-256 后写入
  SPIFFS 双槽（`font_11_a.bin` 等），一次性把位图加载进 PSRAM；标题/说明用 11px、服务器卡 13px、指标列表
  S/M/L 用 11/13/15px；失败时回退内置 498 字子集（`ui/src/font_cjk_16.c`）
- 运行时字体增加了内部 RAM 碎片压力：WebSocket 任务栈 5KB、收发缓冲 2KB；API 轮询任务栈放在 PSRAM 以回收内部 RAM

## 性能相关

- 编译选项：`-O2`、CPU 240MHz、32KB 指令缓存（`sdkconfig.defaults`）
- 绘制缓冲：内部 SRAM + DMA，1/12 屏 ×2，避免 PSRAM bounce buffer 导致的 flush 卡死
- 实测（320×480）：整屏重绘 63–74 ms（渲染 55–73 ms + SPI 约 6 ms），64px 对象动画 35 fps，整屏纯色动画 30 fps；数据 apply 1.0–2.1 ms
