# ESP32-S3 LLM 服务监控看板

简体中文 | [English](README.en.md)

把一块 3.5 寸触摸屏做成**本地大模型推理服务**的实时仪表盘。固件支持两路数据源可同时工作：**传统 API 直连**（halogen / gufo / strata / LM Studio / Ollama）与**专用看板 Server**（下发布局/配色并经 WebSocket 推送指标）。组件、多页布局和**三套配色（经典 / STRIX / TUF）**全部在浏览器编辑器里配置，所见即所得。

> 硬件：微雪 **ESP32-S3-Touch-LCD-3.5-C**（ESP32-S3R8，16 MB Flash + 8 MB PSRAM，320×480 ST7796 SPI 屏 + FT6336 触摸）

## 功能

- **动态布局**：服务端下发组件与网格布局，保存后设备热更新，无需重启
- **Web 可视化编辑**：组件库、属性面板、多页与翻页动画，实时指标预览
- **Web 在线配色**：经典 / STRIX / TUF 三套 token（底色、卡片、描边、强调色、圆角），下拉切换即时预览，保存后设备同步渲染
- **双数据源**：可同时配置「传统 API 地址」和「专用 Server 地址」；两台电脑分别提供 API 指标和服务端指标，同一台也不冲突
- **实时指标**：解码/预填 TPS、并发、排队、KV、MTP、累计 token/请求、主机硬件等，直连 API 与服务端 WS 写入同一份指标存储
- **数据源分组**：编辑器里的指标下拉按「传统 API / 专用 Server」分组，布局绑定一眼可见来源
- **桌宠组件**：绑定数值/布尔指标，按阈值在睡眠/待机/开心/忙碌/警戒/离线之间切换程序化动画；忙碌态可另绑 `llm.busy`，阈值在属性面板调整
- **手机配网**：SoftAP + Captive Portal，扫 Wi-Fi 列表并分别填写传统 API 与专用 Server 地址
- **断线自愈**：上电连不上不会卡死，后台持续重连；联网后自动收起配网页
- **字体子集**：服务端按布局文字动态生成 LVGL binfont，设备校验 SHA-256 后加载
- **串口调试**：抓帧成 PNG、状态统计、切页/翻页动画基准
- **性能基准**：整屏约 50 ms（≈20fps），局部动画 47fps；详见下方「性能」

## 支持的数据源

传统 API 由**设备直连**轮询；专用 Server 也会聚合面板/本机系统指标。两路可同时开启，写入同一份指标存储。服务端配置见 [server/README.md](server/README.md)。

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
这些字段由**服务端**补齐。推荐直接使用 halogen-panel（`:8800`）的公开 `/api/status`：
在 `server/config.json` 里把 `panel_url` 指向面板地址即可，无需密钥。

没有面板时，可在跑模型的机器上启动主机监控脚本：

```bash
python3 firmware/tools/host_agent.py --port 8732 --model-match halogen
```

脚本只用标准库：有 NVIDIA 显卡时走 `nvidia-smi`（功耗/温度/占用/显存），否则读 Linux `hwmon`
与 `/proc/meminfo`。服务端会按需启用 system / gpu 适配器，缺少对应数据时组件显示「—」。

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
3. 选择 Wi-Fi；按需填写：
   - **① 传统 API 地址**（如 `http://192.168.1.100:8000`）+ 服务类型/密钥，可留空
   - **② 专用看板 Server 地址**（如 `http://192.168.1.100:8080`），可留空
4. 保存，设备自动重启并连接；两个地址可指向同一台或两台电脑

也可以在串口直接写入（见下方调试命令）：

```
w <ssid>|<password>
c <url>|<key>    # 传统 API 地址，c - 清空
d <url>          # 专用 Server 地址，d - 清空
```

### 3. Web 编辑器（配色 / 布局预览）

启动服务端后打开 `http://<PC>:8080/`，在顶部「配色」下拉框切换经典 / STRIX / TUF，
画布会立即按主题 token 刷新；「保存布局」后设备热更新。

字体子集可用无头探针预览（无需烧板）：

```bash
curl -o /tmp/font16.bin "http://127.0.0.1:8080/api/v1/font?profile=320x480"
./render.sh --font /tmp/font16.bin out/font.png "温度曲线 熵排队"
```

## 串口调试命令

| 命令 | 作用 |
| --- | --- |
| `s` | 抓一帧，PNG base64 输出到串口 |
| `h` | 打印 FPS / 堆 / PSRAM / 内部 RAM / Wi-Fi / IP / RSSI |
| `p <n>` | 切换布局页面 |
| `v <0-6>` | 临时切换翻页动画（不落盘） |
| `b` | 测整屏刷新耗时（含 SPI） |
| `B` | 分离渲染与传输：`render=… full=… transfer~…` |
| `C` | 空白屏整屏耗时（SPI + 填充基线） |
| `a` / `A` | 小区域 / 整屏动画持续帧率（5 秒） |
| `w <ssid>\|<pass>` | 写入 Wi-Fi 并重启 |
| `c <url>\|<key>` | 写入传统 API 地址/密钥并重启，`c -` 清空 |
| `o <url>` | 写入主机监控地址（可填 8800 面板），`o -` 清空 |
| `d <url>` | 写入看板服务地址并重启，`d -` 清空 |
| `m` | 打印当前指标存储 |
| `P` | 请求进入配网模式 |

配套抓帧脚本（依赖 IDF 自带的 pyserial）：

```bash
~/.espressif/python_env/idf5.5_py3.12_env/bin/python firmware/tools/capture.py \
    --cmd "p 2" --out /tmp/board.png
```

## 0.2 动态看板

`server/` 负责布局/配色编辑与服务端指标聚合；ESP32 负责渲染，并可直接轮询传统 API。

- 配网页分别填写传统 API 与专用 Server；也可串口执行 `c` / `d`
- 设备启动时拉取服务端 `resolved` 布局（含 `theme_tokens`），通过 WebSocket 订阅布局用到的指标；同时若有传统 API 地址，会独立轮询并写入同一指标存储
- **配色在 Web 编辑器里配置**：`shared/catalog/themes.json` 定义三套 token，服务端解析布局时注入，固件 `ui_dynamic` 应用；设备端不再保存主题
- 已支持状态条、KPI、服务器卡、指标列表和隐藏标题
- 布局 v2 多页支持触摸滑动，串口 `p <n>` 可切页；`page_animation` 支持 `slide`（原生跟手）、`fade`（快照交叉淡出）、`stack`（层叠）、`zoom`（缩放）、`flip`（翻转）、`rotate`（旋转）、`none`（直切）。除 `slide` 外都在松手时按位移判定并关闭原生滚动
- 过渡效果合计代码 <2KB；固件 1.86MB / 4MB 分区，剩余约 2.25MB
- 总组件数上限 **20**（实测内部 RAM 约束，见下）
- 服务端保存布局后设备**热更新，无需重启**；≤10 组件时原子换屏（构建期间旧界面保持可见），更多组件仍会短暂黑屏
- 编辑器收到 `layout_changed` 会自动重载；有未保存修改时提示先点「重载」，避免旧状态覆盖新布局
- 布局未变化时（含 WS 重连后的重拉）按哈希跳过重建，避免黑屏闪烁
- **字体子集**：服务端按布局实际文字生成 LVGL binfont（`/api/v1/font`），设备校验 SHA-256 后写入 SPIFFS 双槽并把位图加载进 PSRAM；布局里的任意中文都能显示，没有可用字体时回退内置 498 字子集
- **长按 BOOT（GPIO0）3 秒**进入配网（APSTA），屏幕显示热点/密码/地址；串口 `P` 也可触发用于调试
- STRIX/TUF 独立 UI 已移除，主题统一走 Web 配置；传统 API 直连采集保留，并与 Server 数据并存

启动服务和编辑布局见 [server/README.md](server/README.md)。换电脑接力开发请先看 [docs/0.2-HANDOFF.md](docs/0.2-HANDOFF.md)。

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
├── ui/                 # 共享 UI：配色 token、通用控件、动态布局渲染（不含驱动）
│   ├── include/ui.h
│   ├── include/ui_dynamic.h
│   ├── include/dash_metric_store.h
│   └── src/{ui_widgets,ui_dynamic,dash_metric_store,font_cjk_16}.c
├── shared/             # 服务端/固件共享契约：指标、组件、profile、主题、布局预设
├── server/             # FastAPI 聚合服务 + Web 布局/配色编辑器
├── sim/                # PC 无头字体探针（binfont → PNG）
├── firmware/           # ESP-IDF 工程
│   ├── main/           # 显示/触摸驱动、Wi-Fi、配网、动态看板客户端、调试台
│   ├── components/     # 板载 ST7796 / FT6336 驱动（随仓库提供）
│   ├── tools/          # capture.py 抓帧、host_agent.py 主机监控
│   ├── docs/DEBUGGING.md
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── docs/images/        # README 截图（0.1 直连版）
├── render.sh           # 字体探针一键出图
└── LICENSE
```

`ui/` 被固件引用（`firmware/main/CMakeLists.txt` 中的 `../../ui`），Web 编辑器与服务端共用 `shared/` 目录，保证「编辑器预览」与「设备渲染」使用同一套布局和主题 token。

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

- 整屏重绘约 50 ms（≈20fps），局部更新 47fps；切页/换配色都是一次性全屏绘制
- 服务端缺少对应数据源时，相关组件显示「—」
- 两路数据源写同一份指标存储，同 ID 以最后写入为准；跨机监控时建议 API 指标绑定 `llm.*` / `host.*`，服务端本机指标绑定 `server.*`
- 中文字体内置 498 个常用字（Noto Sans SC，OFL，`ui/src/font_cjk_16.c`），其余文字依赖服务端字体子集；无可用字体时回退内置子集
- `sim/` 使用 LVGL 9.3 做字体探针，固件使用 LVGL 9.6；两者 API 兼容，但会打印少量弃用警告
- 保存新配色会重建 LVGL screen，耗时数百毫秒，属于一次性成本

## License

[MIT](LICENSE)
