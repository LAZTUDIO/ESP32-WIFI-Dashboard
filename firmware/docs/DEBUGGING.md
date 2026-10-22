# ESP32-S3 + LVGL 调试手册（先按这个来）

> 本文件是**流程优先级最高**的记录。遇到 UI / 显示 / 触摸 / 刷屏相关问题时，
> 先按本文的「三层闭环」和「踩坑表」排查，不要重新摸索。
> 全部条目来自 2026-10 在 Waveshare ESP32-S3-Touch-LCD-3.5-C 上的实测。

---

## 0. 三层调试闭环（按成本从低到高，能上一层解决就别开下一层）

| 层 | 手段 | 能查什么 | 成本 |
|---|---|---|---|
| **L1** | PC 模拟器 `./render.sh out/dashboard.png` 出图 | 布局错位、重叠、裁剪、配色、文字截断、控件尺寸 | 秒级，不用板子 |
| **L2** | 串口日志 + 周期性 `PERF` 数值 | FPS、free heap、PSRAM、崩溃/重启、初始化顺序 | 几秒一次，客观数值 |
| **L3** | 串口抓帧 `s` → PNG base64 | 真实色彩（BGR/反色）、SPI 刷屏观感、面板方向 | 0.15~1.1 s/帧 |

判定原则：

- **只有视觉布局问题** → 停在 L1。不要在板子上反复烧写试布局。
- **卡顿 / 重启 / 内存** → 看 L2 的 `PERF`，**不要靠肉眼猜帧率**。
- **颜色不对 / 画面撕裂 / 反色 / 镜像** → 才上 L3，这类问题模拟器永远看不出来。

---

## 1. 环境事实（别再重新探测）

- 本机**没有** Homebrew、**没有** SDL2、**没有** Node/npm。
- 但有完整 Xcode Command Line Tools：`cc -framework Cocoa` 可直接编译原生窗口程序。
  → 所以 PC 模拟器用 **Objective-C + Cocoa**，不用 LVGL 官方 SDL 方案。
- GitHub 直连极慢，LVGL 源码用 Gitee 镜像：
  `https://gitee.com/mirrors/lvgl/repository/archive/release/v9.3.zip`
- 工具链：ESP-IDF **v5.5.6** @ `~/esp/esp-idf`，LVGL **9.6**（`managed_components/lvgl__lvgl`），
  esp_lvgl_port 2.9。
- 每个新终端先 `get_idf`（已写入 `~/.zshrc` 的 alias）。
- 板级组件库在 `~/esp/board_libs/esp32s3-touch-lcd-3.5c/`；其中 `esp_jpeg` **只有解码没有编码**，
  所以截图编码用 LVGL 自带 `lodepng`（`CONFIG_LV_USE_LODEPNG=y`）。

---

## 2. 串口命令通路（最容易白查半天的一类）

### 2.1 主控制台必须是 USB-Serial-JTAG，副控制台「只出不进」

**症状**：日志能从 USB 口正常看到，但 `fgets(stdin)` 永远收不到主机发的命令，
连 `unknown command` 都不打印。

**根因**：`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` 时主控制台是 **UART0**，
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 只是**副控制台**——
它只把日志镜像出去，**不接收输入**。`stdin` 实际绑在 UART0 的 RX 上，而板子的 USB 口是 JTAG 外设。

**正确配置**（`sdkconfig` + `sdkconfig.defaults` 都要改）：

```ini
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_ESP_CONSOLE_UART_NUM=-1
CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM=4
```

同时**删掉冲突项**：`CONFIG_ESP_CONSOLE_UART_DEFAULT`、`CONFIG_ESP_CONSOLE_UART`、
`CONFIG_ESP_CONSOLE_UART_NUM=0`、`CONFIG_ESP_CONSOLE_UART_BAUDRATE`、
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`。

改完 `idf.py reconfigure`，确认 `ESP_CONSOLE_UART_NUM=-1` 再烧。

### 2.2 DTR 必须为 True

**症状**：上位机发命令，板子毫无反应，日志里连一个字符都没有。

**根因**：ESP32-S3 的 USB-Serial-JTAG 在 **DTR 拉低**时认为主机断开，会**丢弃主机发来的数据**。

**正确做法**（pyserial）：

```python
# 复位用 RTS，不要碰 DTR
s.setDTR(False); s.setRTS(True);  time.sleep(0.15)
s.setRTS(False);                  time.sleep(1.2)
s.setDTR(True);  s.setRTS(False); time.sleep(0.3)   # 关键：DTR=True
s.reset_input_buffer()
```

### 2.3 抓帧走 base64，不走裸二进制

UART VFS 可能对 CR/LF 做转换，裸二进制会被污染。
PNG ≈ 10 KB → base64 ≈ 13 KB。115200 下 ~1.1 s/帧，921600 下 ~0.15 s/帧。
协议：`---SNAP-BEGIN w= h= bytes=---` … 76 列换行的 base64 … `---SNAP-END---`。

---

## 3. LVGL 9.6 API 变更（编译报错先查这里）

| 旧写法 | 9.6 正确写法 | 说明 |
|---|---|---|
| `lv_indev_get_point(dev, &p)` 当返回值用 | `lv_indev_get_state(dev) == LV_INDEV_STATE_PRESSED` + `lv_indev_get_point()`（返回 void） | 函数改成 void，`error: void value not ignored` |
| `lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN)` | `lv_obj_set_hidden(o, true)` | add/remove_flag 已废弃 |
| `lv_snapshot_free(snap)` | `lv_draw_buf_destroy(snap)` | `lv_snapshot_take()` 现在返回 `lv_draw_buf_t*` |
| `lv_obj_set_bg_color(o, c)` | `lv_obj_set_style_bg_color(o, c, 0)` | 从来就没有两参版本 |
| 直接 sin/cos 平滑曲线、坐标轴刻度 | **9.6 没有这些 API** | 需要自己用 `lv_line` 采点画 |

### 3.1 `lv_conf_template.h` 整体被 `#if 0` 包住

复制模板后**必须把开头的 `#if 0` 改成 `#if 1`**，否则所有字体/控件开关都不生效。
这个坑踩过两次（固件一次、模拟器 `sim/lv_conf.h` 一次）。

### 3.2 LVGL 9.x 能力边界

- **不再内置图标字体**：FontAwesome 已移除，`lv_symbol_def.h` 只剩码点没有字形。
  图标要么自己画（`lv_line`/`lv_arc`），要么导入字体。
- 内置 Montserrat **无 CJK 字形**：界面文案必须纯 ASCII，中文要自己转子集字体或开 `LV_USE_FREETYPE`。

---

## 4. 布局规范（PC 出图一照就现形）

1. **上下排列的「标签 + 数值」一律用 flex 列容器**，不要用 `lv_obj_align_to`。
   `lv_obj_align_to` 的 align 描述的是**对象自身对齐点**，`LV_ALIGN_BOTTOM_LEFT`
   会把数值左下角对到标签左下角 → 数值往**上**叠，正好压住标签。
   正确写法：

   ```c
   static lv_obj_t *make_hud_cell(lv_obj_t *parent, const char *caption, ...)
   {
       lv_obj_t *col = make_plain(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_SURFACE);
       lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
       lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, cross, cross);
       lv_obj_set_style_pad_row(col, 0, 0);
       /* 再放 caption 和 value 两个 label */
   }
   ```
   这个坑也踩过两次：第一次只调内边距/偏移没修好，第二次换 flex 才彻底解决。

2. **先算纵向预算表再写代码**。按 `上边距 + 每块显式高度 + 块间距 + 内边距` 累加，
   超了就裁结构，不要指望自动布局兜底。曾因预算超 30 px 导致底部卡片被裁。

3. **容器默认宽度是 100%**，把多个子项塞进同一行时，子项要显式
   `lv_obj_set_size(child, LV_SIZE_CONTENT, LV_SIZE_CONTENT)`，否则后面的被挤出屏幕。

4. **重构时容易丢 `lv_obj_align`**：控件创建了没对齐就会跑到 (0,0) 压住别的东西。
   每次重构后跑一次 L1 出图。

---

## 5. 模拟器（原生 Cocoa）

- `NSBitmapImageRep` **不要用外部 buffer 初始化**——它并不真正采用那块内存，
  `bitmapData` 指向自己分配的全零存储，于是**窗口全黑**。
  正确：`initWithBitmapDataPlanes:NULL ...` 让它自分配，每帧
  `memcpy([s_bitmap bitmapData], s_fb888, w*h*3)` 再 `setNeedsDisplay:YES`。
  加一句自检打印（比较前 12 字节）确认 copy 真的生效。
- 参数：`bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO
  colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:960 bitsPerPixel:24`（320×480）。
- `NSTimer` 必须挂到 `NSRunLoopCommonModes`，否则拖窗口/拖鼠标时动画停摆。
- 鼠标 y 轴要翻转：`y = DISP_H - p.y / WIN_SCALE`；`mouseDown/Dragged` 置按下，
  `mouseUp/mouseExited` 置释放。
- 无头出图用 `--dump <bmp>`；怀疑渲染链路本身坏了先用 `--red` 纯色测试。
- 本机 `screencapture` 无屏幕录制权限（`could not create image from display`），
  无法截窗口，**只能靠 `--dump` + 进程 CPU 时间累积 + 内存自检间接确认**。

---

## 6. 性能约束（先算再画）

- 320×480×2 = **307 200 B/帧**；SPI @80 MHz 全屏约 **60 ms → 约 15 FPS 上限**。
- 因此：布局扁平化，**少用阴影、半透明、大面积渐变**，能局部 `lv_obj_invalidate`
  就不要整屏重绘。
- `esp_lvgl_port` 里 `flags.sw_rotate = 1` 会让 `lvgl_port_disp_rotation_update()`
  直接 return，方向参数**不会**写进 MADCTL。本工程用 `sw_rotate = 0`。
- **静止画面 `fps=0.0` 是正常的**：LVGL 不重绘。要测帧率先让画面动起来
  （调试命令 `r` 发射球）。

---

## 7. 板级已验证事实（别再试错）

| 项 | 值 |
|---|---|
| 屏幕 | ST7796 320×480 RGB565，SPI，**BGR + 反色 + mirror_x** |
| LCD SPI | MOSI=GPIO1，SCLK=GPIO5，DC=GPIO3，**CS/RST 均未接** |
| LCD 复位 | 经 **TCA9554**（I2C 0x20）第 1 脚，拉低 120 ms 再释放 |
| 背光 | GPIO6，LEDC 5 kHz，默认 80% |
| 触摸 | FT6336，I2C **0x38**，SDA=GPIO8 / SCL=GPIO7（与 TCA9554 共用） |
| 触摸坐标 | 与屏幕一致，`swap_xy/mirror_*` 全关 |
| 芯片 | ESP32-S3R8，240 MHz，8 MB Octal PSRAM @80 MHz，16 MB Flash |
| 其它外设 | AXP2101 PMU(0x34)、QMI8658(0x6A)、PCF85063 RTC(0x51)、ES8311(0x18) |

---

## 8. 命令速查

```bash
# 编译 + 烧写（端口按实际改）
. ~/esp/esp-idf/export.sh && cd firmware && idf.py -p /dev/cu.usbmodem21401 flash

# PC 模拟器（无头出图，改布局不用烧板）
./render.sh out/dashboard.png

# 串口抓帧（板子端命令 s）
python3 tools/capture.py --out /tmp/board.png           # 抓一帧当前画面
python3 tools/capture.py --send h                       # 只发命令看日志
python3 tools/capture.py --cmd "p 2"                    # 先切到第 2 页再抓帧
```

固件调试命令：`?` 帮助 / `h` 状态 / `s` 抓帧 / `p N` 切页 /
`w <ssid>|<pass>` 串口配网 / `c <url>|<key>` 配服务地址。

---

## 9. 新问题的标准处理顺序

1. 先在 L1 出图确认是不是布局/配色问题，是就修，**不要烧板**。
2. 不是视觉问题 → 读 `PERF` 数值，看 heap 是否持续下降、FPS 是否对得上预算。
3. 颜色/方向/撕裂 → L3 抓一帧 PNG 比对。
4. 改完先 `idf.py build`，再烧，再抓日志确认启动顺序：
   `backlight on GPIO6` → `ST7796 ... ready` → `FT6336 touch ready` → `dash_app` 开始轮询。
5. 任何结论都要有证据（图 / 数值 / 日志行），不要凭「看起来像」下判断。
