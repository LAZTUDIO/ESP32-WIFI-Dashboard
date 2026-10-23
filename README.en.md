# ESP32-S3 LLM Server Dashboard

English | [简体中文](README.md)

A 3.5-inch touch screen dashboard for a local LLM inference server. The firmware supports two data paths at the same time: **direct legacy API polling** (halogen / gufo / strata / LM Studio / Ollama) and a **dedicated dashboard server** (layout/theme + WebSocket metrics). Widgets, multi-page layouts and **three colour themes (Classic / STRIX / TUF)** are all configured in the browser editor with WYSIWYG preview.

> Hardware: Waveshare **ESP32-S3-Touch-LCD-3.5-C** (ESP32-S3R8, 16 MB flash + 8 MB PSRAM, 320×480 ST7796 SPI panel + FT6336 touch)

## Features

- **Dynamic layout** — the server pushes widget/grid layouts and the device hot-reloads them without rebooting
- **Web visual editor** — widget palette, property inspector, multiple pages and page animations, live metric preview
- **Web colour themes** — Classic / STRIX / TUF token sets (background, card, border, accents, radius); switch in the dropdown for instant preview, save to sync the device
- **Dual data sources** — configure both a legacy API URL and a dashboard server URL; two machines can provide API and server metrics, or both can point at one machine
- **Live metrics** — decode/prefill TPS, concurrency, queue depth, KV usage, MTP, token totals and host hardware; direct API and server WebSocket write into the same metric store
- **Source grouping** — metric dropdowns in the editor are grouped as “Traditional API” / “Dedicated Server”
- **Desktop pet widget** — bind a numeric/boolean metric and switch between sleep/idle/happy/busy/alert/offline procedural animations; the busy state can bind `llm.busy`, and thresholds are editable in the inspector
- **Phone provisioning** — SoftAP + captive portal with Wi-Fi scan plus separate traditional API and dashboard server fields
- **Self-healing Wi-Fi** — keeps retrying in the background after a failed boot connect, and hides the provisioning overlay once online
- **Font subsets** — the server generates an LVGL binfont from the layout text; the device verifies SHA-256 before loading it
- **Serial console** — frame capture, stats, page/animation benchmarks
- **Performance** — ~50 ms full-screen redraw (~20 fps), 47 fps partial updates; see “Performance” below

## Supported data sources

Traditional APIs are polled **directly by the device**; the dedicated server also aggregates panel/host-system metrics. Both paths can run at once and write into the same metric store. See [server/README.md](server/README.md) for server configuration.

| Service | Probe endpoints | Metrics available |
| --- | --- | --- |
| **halogen-panel (8800)** | `/api/status` | model/version, live & average TPS, slots/context, in_flight/queued, KV, MTP, GPU/RAM/uptime (public, no key) |
| **halogen** | `/health` + `/metrics` | model/version, slots, context, in_flight/queued, KV pool, token counters, MTP, cache |
| **gufo** | `/health` + `/v1/slots` | same as above; falls back to `n_decoded` deltas when `/metrics` is absent |
| **strata** | `/api/monitor` | outputTps / promptTps / queue / context / hardware.* / totals.* / engine start time |
| **LM Studio** | `/api/v0/models` | model, quantisation, context |
| **Ollama** | `/api/ps` | loaded models, size, context |
| **OpenAI-compatible** | `/v1/models` | model list |

The provider defaults to `auto` (probed in the order above) and can be pinned in the provisioning page (`panel` for port 8800).
The panel's `/api/usage`, `/api/params`, `/api/logs` and TTS/embedding endpoints remain password-protected and are never read by the firmware.

> TPS note: gauges such as `llamacpp:predicted_tokens_seconds` read 0 on some servers, so the dashboard always derives live speed from the deltas of `tokens_predicted_total` and `tokens_predicted_seconds_total`.

## Where do hardware metrics come from?

LLM APIs do **not** expose GPU power, temperature, utilisation or RAM (halogen's `/metrics` contains inference counters only), so the **server** fills those in. The recommended source is halogen-panel's public `/api/status` on `:8800`: point `panel_url` in `server/config.json` at it, no key required.

Without a panel, run the host agent on the machine that runs the model:

```bash
python3 firmware/tools/host_agent.py --port 8732 --model-match halogen
```

The agent uses the standard library only: with an NVIDIA GPU it shells out to `nvidia-smi` (power/temperature/utilisation/VRAM), otherwise it reads Linux `hwmon` and `/proc/meminfo`. Widgets show “—” when a source is unavailable.

## Quick start

### 1. Build and flash the firmware

Requires ESP-IDF v5.5.x (validated on v5.5.6):

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem21401 flash
```

The first build downloads LVGL 9.6, `esp_lvgl_port` and friends through the component manager (`main/idf_component.yml`).

### 2. Provision

1. Join the `ESP32-LLM-Setup` hotspot (password `llm123456`)
2. Open `http://192.168.4.1` (the captive portal usually pops up by itself)
3. Pick your Wi-Fi and optionally fill in:
   - **① Traditional API** (e.g. `http://192.168.1.100:8000`) with provider/key, may be empty
   - **② Dashboard server** (e.g. `http://192.168.1.100:8080`), may be empty
4. Save — the board reboots and connects; the two URLs may point at one machine or two

Serial provisioning is also available:

```
w <ssid>|<password>
c <url>|<key>    # traditional API URL; c - clears it
d <url>          # dashboard server URL; d - clears it
```

### 3. Web editor (theme / layout preview)

Start the server and open `http://<PC>:8080/`. The 配色 dropdown switches Classic / STRIX / TUF and repaints the canvas with the same tokens the device uses; “Save layout” hot-reloads the device.

Preview the font subset with the headless probe:

```bash
curl -o /tmp/font16.bin "http://127.0.0.1:8080/api/v1/font?profile=320x480"
./render.sh --font /tmp/font16.bin out/font.png "温度曲线 熵排队"
```

## Serial console commands

| Command | Action |
| --- | --- |
| `s` | Capture one frame, output as base64 PNG |
| `h` | Print FPS / heap / PSRAM / internal RAM / Wi-Fi / IP / RSSI |
| `p <n>` | Switch layout page |
| `v <0-6>` | Temporarily change page animation (not persisted) |
| `b` | Benchmark a full-screen refresh (including SPI) |
| `B` | Split render/transfer: `render=… full=… transfer~…` |
| `C` | Blank-screen baseline (fill + SPI) |
| `a` / `A` | Small-area / full-screen animation FPS over 5 s |
| `w <ssid>\|<pass>` | Write Wi-Fi credentials and reboot |
| `c <url>\|<key>` | Write the traditional API URL/key and reboot; `c -` clears it |
| `o <url>` | Write the host monitor URL (or 8800 panel); `o -` clears it |
| `d <url>` | Write the dashboard service URL and reboot; `d -` clears it |
| `m` | Dump the metric store |
| `P` | Request provisioning mode |

Companion capture script (uses the pyserial bundled with ESP-IDF):

```bash
~/.espressif/python_env/idf5.5_py3.12_env/bin/python firmware/tools/capture.py \
    --cmd "p 2" --out /tmp/board.png
```

## Performance

Measured on the board (480×320 logical resolution):

| Scenario | Result |
| --- | --- |
| Blank full-screen fill + SPI | 31 ms |
| Real dashboard full redraw | 50–53 ms (42 ms render + 7 ms SPI, ~20 fps) |
| Partial / full-screen solid animation | 47 fps |
| UI data apply | < 30 ms |
| Free internal RAM | 33–38 KB |

Key settings (`firmware/sdkconfig.defaults` and `firmware/main/display.c`):

- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (do not keep the default `-Og`)
- 240 MHz CPU + **32 KB instruction cache** (16 KB makes rendering 3× slower)
- LVGL draw buffers in internal SRAM with DMA, 1/12 screen ×2, `max_transfer_sz` sized to one chunk
- `LV_COLOR_FORMAT_RGB565_SWAPPED`, removing the 300 KB/frame CPU byte swap
- `LV_DEF_REFR_PERIOD=16` for snappier touch and small-area updates

> A 480×320 RGB565 frame is 300 KB; 80 MHz SPI caps out around 32 fps. A complex dashboard
> full redraw at ~20 fps is expected, while partial updates reach 47 fps. A full-frame PSRAM
> buffer with direct DMA is not viable for SPI panels on ESP32-S3 (DMA cannot read PSRAM).

## Repository layout

```
.
├── ui/                 # Shared UI: theme tokens, widgets, dynamic renderer (no drivers)
│   ├── include/ui.h
│   ├── include/ui_dynamic.h
│   ├── include/dash_metric_store.h
│   └── src/{ui_widgets,ui_dynamic,dash_metric_store,font_cjk_16}.c
├── shared/             # Contract shared by server/firmware: metrics, widgets, profiles, themes
├── server/             # FastAPI aggregator + web layout/theme editor
├── sim/                # Headless font probe (binfont → PNG)
├── firmware/           # ESP-IDF project
│   ├── main/           # display/touch drivers, Wi-Fi, portal, dashboard client, console
│   ├── components/     # on-board ST7796 / FT6336 drivers
│   ├── tools/          # capture.py, host_agent.py
│   ├── docs/DEBUGGING.md
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── docs/images/        # README screenshots (0.1 direct mode)
├── render.sh           # one-shot font probe render
└── LICENSE
```

`ui/` is compiled by the firmware (`firmware/main/CMakeLists.txt` references `../../ui`), while the web editor and server share `shared/`, so the editor preview and the device render use the same layout and theme tokens.

## Board notes

| Function | Pin / setting |
| --- | --- |
| LCD SPI | MOSI=GPIO1, SCLK=GPIO5, DC=GPIO3, **CS/RST not wired** |
| LCD reset | via TCA9554 (I2C 0x20) pin 1, driven low for 120 ms at boot |
| Backlight | GPIO6, LEDC PWM @ 5 kHz |
| Touch | FT6336 (I2C 0x38), SDA=GPIO8, SCL=GPIO7 |
| Panel | 320×480, RGB565, BGR + inverted + `mirror_x`, `sw_rotate=0` |

Bring-up notes, PSRAM framebuffer strategy and serial capture troubleshooting live in
[firmware/docs/DEBUGGING.md](firmware/docs/DEBUGGING.md).

## Known limitations

- A full-screen redraw costs ~50 ms (~20 fps); partial updates reach 47 fps. Page changes and colour switches are one-off full draws
- Widgets show “—” when the corresponding server data source is unavailable
- Both paths write into one metric store; for a given ID the last writer wins. When monitoring two machines, bind API metrics to `llm.*` / `host.*` and server-local metrics to `server.*`
- The bundled CJK font covers 498 common characters (`ui/src/font_cjk_16.c`); other text comes from the server font subset, falling back to the bundled font when unavailable
- `sim/` uses LVGL 9.3 for the font probe while the firmware uses LVGL 9.6; the APIs are compatible but a few deprecation warnings appear
- Saving a new colour theme rebuilds the LVGL screen, which takes a few hundred milliseconds

## License

[MIT](LICENSE)
