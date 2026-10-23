# ESP32-S3 LLM 服务监控看板

简体中文 | [English](README.en.md)

把一块 3.5 寸触摸屏做成**本地大模型推理服务**的实时仪表盘：Wi-Fi 联网后轮询本机 LLM API，展示 TPS、并发、排队、KV 占用、MTP 接受率、用量与历史曲线；手机扫码式配网，支持 halogen / gufo / strata / LM Studio / Ollama 等主流服务，也可直接接入 **halogen-panel（8800）** 的公开接口，无需密钥。

内置 **经典 / STRIX 横屏 / TUF 竖屏** 三套主题，可在设置页直接切换并支持 180° 翻转；UI 代码同时编译到 PC 无头模拟器，改布局不用反复烧板。

> 硬件：微雪 **ESP32-S3-Touch-LCD-3.5-C**（ESP32-S3R8，16 MB Flash + 8 MB PSRAM，320×480 ST7796 SPI 屏 + FT6336 触摸）

## 界面

| 概览 | 系统 | 设置 |
| --- | --- | --- |
| ![概览](docs/images/overview.png) | ![系统](docs/images/system.png) | ![设置](docs/images/settings.png) |

两套电竞副屏风格主题（设置页可切换，STRIX 为 480×320 横屏）：

| STRIX（横屏 ROG 风） | TUF（竖屏军工风） | 主题与方向设置 |
| --- | --- | --- |
| ![STRIX](docs/images/theme-strix.png) | ![TUF](docs/images/theme-tuf.png) | ![主题设置](docs/images/theme-switch.png) |

配网浮层（无 Wi-Fi 时自动出现）：

![配网](docs/images/provisioning.png)

## 功能

- **实时指标**：解码/预填 TPS（区间差分，非 gauge）、平均与峰值速度、并发数、排队数、KV 占用、MTP 接受率、累计 token/请求、提示缓存命中与容量
- **面板直连**：可直接读取 halogen-panel `:8800` 的公开 `/api/status`（无需密钥），一次拿到模型/版本、TPS、KV、并发与硬件数据；也可把面板地址填进「主机监控」，为 8731 主源补齐缺失指标
- **历史曲线**：速度 / KV / 并发 / 功耗四类曲线，触摸切换
- **系统页**：功耗、GPU 温度、GPU 占用、内存、主机/模型/引擎运行时长、设备运行时长
- **手机配网**：SoftAP + Captive Portal，扫 Wi-Fi 列表、填服务地址与密钥，保存后自动重启联网
- **断线自愈**：上电连不上不会卡死，后台持续重连；联网后自动收起配网页
- **三套主题**：经典（320×480）/ STRIX（480×320 横屏）/ TUF（320×480），设置页切换、NVS 持久化
- **屏幕方向**：主题默认方向 + 180° 翻转，运行时硬件旋转，无需重刷固件
- **PC 无头模拟器**：不用烧板就能出图迭代布局（同一份 UI 源码），内置越界/重叠/文字溢出/对比度自动巡检
- **串口调试**：抓帧成 PNG、状态统计、切页/切主题、渲染耗时与动画帧率基准
- **性能基准**：整屏约 50 ms（≈20fps），局部动画 47fps；详见下方「性能」

## 支持的 API

| 服务 | 探测端点 | 能取到的指标 |
| --- | --- | --- |
| **halogen-panel (8800)** | `/api/status` | 模型/版本、实时与均值 TPS、slots/上下文、in_flight/queued、KV、MTP、GPU/内存/uptime（公开，无需密钥） |
| **halogen** | `/health` + `/metrics` | 模型/版本、slots、上下文、in_flight/queued、KV 池、token 计数器、MTP、缓存 |
| **gufo** | `/health` + `/v1/slots` | 同上；无 metrics 时用 slots 的 `n_decoded` 差分 |
| **strata** | `/api/monitor` | outputTps / promptTps / queue / context / hardware.* / totals.* / 引擎启动时间 |
| **LM Studio** | `/api/v0/models` | 模型、量化、上下文 |
| **Ollama** | `/api/ps` | 已加载模型、大小、上下文 |
| **OpenAI 兼容** | `/v1/models` | 模型列表 |

服务类型默认 `auto`，按上表顺序探测；也可在配网页手动指定（面板选 `panel`）。
面板的 `/api/usage`、`/api/params`、`/api/logs` 与 TTS/向量接口仍受面板密码保护，固件不会读取。

> TPS 说明：`llamacpp:predicted_tokens_seconds` 这类 gauge 在部分服务里恒为 0，因此本项目统一用
> `tokens_predicted_total` 与 `tokens_predicted_seconds_total` 的区间差分计算实时速度。

## 硬件指标从哪来

LLM API **不提供** GPU 功耗、温度、占用率、内存等数据（halogen 的 `/metrics` 全是推理计数器），
这些字段由主机侧的小脚本提供。在跑模型的机器上执行：

```bash
python3 firmware/tools/host_agent.py --port 8732 --model-match halogen
```

然后配网页填「主机监控地址」`http://<主机IP>:8732`。脚本只用标准库：有 NVIDIA 显卡时走 `nvidia-smi`
（功耗/温度/占用/显存），否则读 Linux `hwmon` 与 `/proc/meminfo`。不填也能用，系统页对应卡片显示「无数据」。

如果机器上已经跑了 **halogen-panel（`:8800`）**，不必再启动 `host_agent.py`：把「主机监控地址」
直接填面板地址 `http://<IP>:8800` 即可。固件会先尝试 `/stats`，失败后自动改用面板的 `/api/status`，
补齐功耗、温度、GPU、内存与 uptime；当主源（如 8731）的 `/metrics` 需要密钥时，TPS / KV / MTP
也会一并从面板补齐。若只想用面板当唯一数据源，把「API 服务地址」设为 `http://<IP>:8800`、
服务类型选 `panel`、密钥留空即可。

## 快速开始

### 1. 编译烧录固件

需要 ESP-IDF v5.5.x（本项目在 v5.5.6 上验证）：

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem21401 flash
```

首次构建会由组件管理器自动下载 LVGL 9.6、`esp_lvgl_port` 等依赖（`main/idf_component.yml`）。

### 2. 配网

1. 手机连接热点 `ESP32-LLM-Setup`（密码 `llm123456`）
2. 浏览器打开 `http://192.168.4.1`（Captive Portal 会自动弹出）
3. 选择 Wi-Fi、填写 API 服务地址与密钥，可选填主机监控地址
4. 保存，设备自动重启并连接

也可以在串口直接写入（见下方调试命令）：

```
w <ssid>|<password>
c <url>|<api-key>
o <url>          # 可选：主机监控地址，也可直接填 8800 面板
```

### 3. PC 模拟器预览（可选）

```bash
./sim/fetch_lvgl.sh          # 拉取 LVGL 9.3 源码到 lvgl/
./render.sh out/dashboard.png

# 指定页面 / 数据场景 / 主题 / 180° 翻转
./render.sh out/strix.png 0 live 1 0      # STRIX 监控页
./render.sh out/tuf_trend.png 1 live 2 0  # TUF 曲线页
```

`render.sh` 用无头渲染输出 PNG，改 UI 时不必反复烧板。参数：

```
render.sh <out.png> [page] [scenario] [theme] [rot180]
  page      经典 0-3；STRIX 0-1；TUF 0-2
  scenario  live | offline | empty
  theme     0=经典 1=STRIX 2=TUF
  rot180    0/1
```

每次渲染还会自动巡检越界、同级重叠、文字溢出和低对比度。

## 串口调试命令

| 命令 | 作用 |
| --- | --- |
| `s` | 抓一帧，PNG base64 输出到串口 |
| `h` | 打印 FPS / 堆 / PSRAM / 内部 RAM / Wi-Fi / IP / RSSI |
| `p <n>` | 切换页面（编号随主题不同） |
| `t <0-2>` | 切换主题（0 经典 / 1 STRIX / 2 TUF） |
| `r` | 翻转 180° |
| `b` | 测整屏刷新耗时（含 SPI） |
| `B` | 分离渲染与传输：`render=… full=… transfer~…` |
| `C` | 空白屏整屏耗时（SPI + 填充基线） |
| `a` / `A` | 小区域 / 整屏动画持续帧率（5 秒） |
| `w <ssid>\|<pass>` | 写入 Wi-Fi 并重启 |
| `c <url>\|<key>` | 写入 API 服务地址与密钥并重启 |
| `o <url>` | 写入主机监控地址（也可填 8800 面板）并重启，`o -` 清空 |
| `d <url>` | 写入 Dashboard 服务地址并重启，`d -` 清空（回到直连模式） |

配套抓帧脚本（依赖 IDF 自带的 pyserial）：

```bash
~/.espressif/python_env/idf5.5_py3.12_env/bin/python firmware/tools/capture.py \
    --cmd "p 2" --out /tmp/board.png
```

## 0.2 动态看板（开发中）

仓库新增 `server/` 动态看板链路：服务端负责采集和布局编辑，ESP32 只负责渲染。

- 配网页填写 Dashboard 服务地址，或串口执行 `d http://<PC>:8080` 启用；`d -` 回到直连模式
- 设备启动时拉取服务端 `resolved` 布局，通过 WebSocket 订阅布局用到的指标
- 已支持状态条、KPI、服务器卡、指标列表和隐藏标题
- 布局 v2 多页支持触摸滑动，串口 `p <n>` 可切页；`page_animation` 支持 `slide`（原生跟手）、`fade`（快照交叉淡出）、`stack`（层叠）、`zoom`（缩放）、`flip`（翻转）、`rotate`（旋转）、`none`（直切）。除 `slide` 外都在松手时按位移判定并关闭原生滚动
- 过渡效果合计代码 <2KB；固件 1.86MB / 4MB 分区，剩余约 2.25MB
- 总组件数上限 **20**（实测内部 RAM 约束，见下）
- 服务端保存布局后设备**热更新，无需重启**；≤10 组件时原子换屏（构建期间旧界面保持可见），更多组件仍会短暂黑屏
- 编辑器收到 `layout_changed` 会自动重载；有未保存修改时提示先点「重载」，避免旧状态覆盖新布局
- 布局未变化时（含 WS 重连后的重拉）按哈希跳过重建，避免黑屏闪烁
- **长按 BOOT（GPIO0）3 秒**进入配网（APSTA），屏幕显示热点/密码/地址；串口 `P` 也可触发用于调试
- 旧版直连 provider 模式保留

启动服务和编辑布局见 [server/README.md](server/README.md)。换电脑接力开发请先看 [docs/0.2-HANDOFF.md](docs/0.2-HANDOFF.md)。完整主题/字体配置包、480×320 横屏、cube/转盘动画在后续里程碑。

## 性能

板子实测（320×480，ST7796 SPI 80MHz，ESP32-S3 240MHz）：

| 场景 | 数值 |
| --- | --- |
| 整屏空白填充 + SPI 传输 | 30–31 ms |
| 真实仪表盘整屏重绘（7–25 组件） | 63–74 ms（渲染 55–73 ms，SPI 传输约 6 ms） |
| 翻页 `slide`（原生跟手 + 松手吸附） | 440–470 ms，约 15 fps |
| 翻页 `fade`（快照交叉淡出，松手触发） | 210–270 ms |
| 翻页 `stack`（旧页缩小下移） | 290–335 ms |
| 翻页 `zoom`（旧页缩成小卡片） | 260–365 ms |
| 翻页 `flip`（旧页横向压扁） | 200–305 ms |
| 翻页 `rotate`（旧页倾斜缩小） | 325–390 ms |
| 翻页 `none`（松手直切） | 0 ms + 一次整屏重绘（约 60–75 ms） |
| 翻页快照捕获 | 20–61 ms，RGB565 约 307KB PSRAM，用完释放 |
| 64px 对象动画 / 整屏纯色动画 | 35 fps / 30 fps |
| 一次数据 apply（全部页面） | 1.0 ms（7 组件）、1.5 ms（15 组件）、2.1 ms（25 组件） |
| 热更新重建 | 13 组件 256 ms；25 组件 480 ms |
| 内部 RAM 空闲 / 最大连续块 | 7 组件 77KB/28KB；19 组件 51KB/7.7KB；25 组件 41KB/3.4KB |

> `slide` 用 LVGL 原生 tileview 滚动，页面跟手但每帧要重绘两页，本板约 15fps；其余效果关闭原生滚动，在松手时按位移判定，用一张 PSRAM 截图做不透明变换（fade 为交叉淡出），旧页只渲染一次，低帧率下也不会出现重影。
> 内部 RAM 随组件数下降且碎片化；25 组件时最大连续块仅 3.4KB，WebSocket 断线重建会失败，故上限设为 20。

关键配置（`firmware/sdkconfig.defaults` 与 `firmware/main/display.c`）：

- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`（不要用默认 `-Og`）
- CPU 240 MHz + **32 KB 指令缓存**（16 KB 时渲染会慢 3 倍）
- LVGL 绘制缓冲放内部 SRAM + DMA，1/12 屏 ×2，`max_transfer_sz` 只覆盖单块
- `LV_COLOR_FORMAT_RGB565_SWAPPED`，省掉每帧 300KB 的 CPU 字节交换
- `LV_DEF_REFR_PERIOD=16`，触摸与小区域更新更跟手

> SPI 屏每帧 300KB，80 MHz 理论上限约 32fps；复杂仪表盘整屏重绘约 20fps 属于合理区间，
> 局部更新可到 47fps。全帧 PSRAM 缓冲 + DMA 在 ESP32-S3 的 SPI 屏上不可行（DMA 不能直接读 PSRAM）。

## 目录结构

```
.
├── ui/                 # 共享 UI：设计 token、控件、经典/STRIX/TUF 主题（不含驱动）
│   ├── include/ui.h
│   ├── include/ui_theme.h
│   ├── include/dash_model.h
│   └── src/{ui_widgets,ui_dashboard,ui_theme_strix,ui_theme_tuf,font_cjk_16}.c
├── sim/                # PC 无头模拟器（LVGL → 内存帧缓冲 → PNG）
│                        # main.c 支持主题/翻转参数，ui_audit.c 做布局巡检
├── firmware/           # ESP-IDF 工程
│   ├── main/           # 显示/触摸驱动、Wi-Fi、配网、指标采集、调试台
│   ├── components/     # 板载 ST7796 / FT6336 驱动（随仓库提供）
│   ├── tools/          # capture.py 抓帧、host_agent.py 主机监控
│   ├── docs/DEBUGGING.md
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── docs/images/        # README 截图
├── render.sh           # 模拟器一键出图
└── LICENSE
```

`ui/` 同时被固件（`firmware/main/CMakeLists.txt` 引用 `../../ui`）和 PC 模拟器使用，保证"模拟器看到的"就是"板子上跑的"。

## 板级要点

| 功能 | 引脚 / 参数 |
| --- | --- |
| LCD SPI | MOSI=GPIO1，SCLK=GPIO5，DC=GPIO3，**CS/RST 未接** |
| LCD 复位 | 经 TCA9554（I2C 0x20）第 1 脚，上电拉低 120 ms 再释放 |
| 背光 | GPIO6，LEDC PWM 5 kHz |
| 触摸 | FT6336（I2C 0x38），SDA=GPIO8，SCL=GPIO7 |
| 面板 | 320×480，RGB565，BGR + 反色 + `mirror_x`，`sw_rotate=0` |

驱动初始化、PSRAM 帧缓冲策略、串口抓帧排障等实测记录见
[firmware/docs/DEBUGGING.md](firmware/docs/DEBUGGING.md)。

## 已知限制

- 整屏重绘约 50 ms（≈20fps），局部更新 47fps；切页/切主题是一次性全屏绘制
- 缺少主机监控脚本时，功耗/温度/GPU/内存与主机 uptime 显示「无数据」；若用 8800 面板代替则无需该脚本
- 面板模式下 `TOKENS` 与 `CACHE HIT` 显示「—」：对应数据在面板登录接口 `/api/usage` 或引擎受保护的 `/metrics` 中
- 中文字体内置 498 个常用字（Noto Sans SC，OFL，`ui/src/font_cjk_16.c`），生僻字会缺字
- `sim/` 使用 LVGL 9.3，固件使用 LVGL 9.6；两者 API 兼容，但模拟器会打印少量弃用警告
- 切换主题会重建 LVGL screen，耗时数百毫秒，属于一次性成本

## License

[MIT](LICENSE)
