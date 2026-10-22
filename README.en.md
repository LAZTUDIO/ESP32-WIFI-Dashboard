# ESP32-S3 LLM Server Dashboard

English | [简体中文](README.md)

A 3.5-inch touch screen dashboard for a local LLM inference server. Once connected to Wi-Fi, the board polls your LLM API and shows decode/prefill TPS, concurrency, queue depth, KV cache usage, MTP acceptance rate, token usage and history charts. Provisioning is done from a phone browser, and the firmware speaks to halogen / gufo / strata / LM Studio / Ollama out of the box.

> Hardware: Waveshare **ESP32-S3-Touch-LCD-3.5-C** (ESP32-S3R8, 16 MB flash + 8 MB PSRAM, 320×480 ST7796 SPI panel + FT6336 touch)

## Screenshots

| Overview | System | Settings |
| --- | --- | --- |
| ![Overview](docs/images/overview.png) | ![System](docs/images/system.png) | ![Settings](docs/images/settings.png) |

Provisioning overlay (shown automatically when Wi-Fi is unavailable):

![Provisioning](docs/images/provisioning.png)

## Features

- **Live metrics** — decode/prefill TPS (computed from counter deltas, not gauges), average & peak speed, in-flight requests, queue depth, KV usage, MTP acceptance, cumulative tokens/requests, prompt-cache hit rate and size
- **History charts** — speed / KV / concurrency / power, switchable by touch
- **System page** — power draw, GPU temperature, GPU utilisation, RAM, host/model/engine uptime, device uptime
- **Phone provisioning** — SoftAP + captive portal with Wi-Fi scan, server URL, API key; saves to NVS and reboots
- **Self-healing Wi-Fi** — keeps retrying in the background after a failed boot connect, and hides the provisioning overlay once online
- **Headless PC simulator** — iterate on layout without flashing the board; the UI sources are shared
- **Serial console** — frame capture as PNG, stats, page switching, serial provisioning

## Supported APIs

| Service | Probe endpoints | Metrics available |
| --- | --- | --- |
| **halogen** | `/health` + `/metrics` | model/version, slots, context, in_flight/queued, KV pool, token counters, MTP, cache |
| **gufo** | `/health` + `/v1/slots` | same as above; falls back to `n_decoded` deltas when `/metrics` is absent |
| **strata** | `/api/monitor` | outputTps / promptTps / queue / context / hardware.* / totals.* / engine start time |
| **LM Studio** | `/api/v0/models` | model, quantisation, context |
| **Ollama** | `/api/ps` | loaded models, size, context |
| **OpenAI-compatible** | `/v1/models` | model list |

The provider defaults to `auto` (probed in the order above) and can be pinned in the provisioning page.

> TPS note: gauges such as `llamacpp:predicted_tokens_seconds` read 0 on some servers, so the dashboard always derives live speed from the deltas of `tokens_predicted_total` and `tokens_predicted_seconds_total`.

## Where do hardware metrics come from?

LLM APIs do **not** expose GPU power, temperature, utilisation or RAM (halogen's `/metrics` contains inference counters only), so those fields come from a small host-side agent. On the machine running the model:

```bash
python3 firmware/tools/host_agent.py --port 8732 --model-match halogen
```

Then fill in the host monitor address `http://<host-ip>:8732` on the provisioning page. The agent uses the standard library only: with an NVIDIA GPU it shells out to `nvidia-smi` (power/temperature/utilisation/VRAM), otherwise it reads Linux `hwmon` and `/proc/meminfo`. Without it the corresponding cards simply show "no data".

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
3. Pick your Wi-Fi, enter the API base URL and key, optionally the host agent address
4. Save — the board reboots and connects

Serial provisioning is also available:

```
w <ssid>|<password>
c <url>|<api-key>
```

### 3. PC simulator (optional)

```bash
./sim/fetch_lvgl.sh          # clones LVGL 9.3 into lvgl/
./render.sh out/dashboard.png
```

`render.sh` renders all four pages to PNG through a headless LVGL framebuffer, so you can iterate on the UI without flashing.

## Serial console commands

| Command | Action |
| --- | --- |
| `s` | Capture one frame, output as base64 PNG |
| `h` | Print FPS / heap / PSRAM / Wi-Fi / IP / RSSI |
| `p <0-3>` | Switch page (0 overview, 1 charts, 2 system, 3 settings) |
| `w <ssid>\|<pass>` | Write Wi-Fi credentials and reboot |
| `c <url>\|<key>` | Write API base URL and key, then reboot |

Companion capture script (uses the pyserial bundled with ESP-IDF):

```bash
~/.espressif/python_env/idf5.5_py3.12_env/bin/python firmware/tools/capture.py \
    --cmd "p 2" --out /tmp/board.png
```

## Repository layout

```
.
├── ui/                 # Shared UI: design tokens, widgets, four pages (no drivers)
│   ├── include/ui.h
│   ├── include/dash_model.h
│   └── src/{ui_widgets,ui_dashboard,font_cjk_16}.c
├── sim/                # Headless PC simulator (LVGL → framebuffer → PNG)
├── firmware/           # ESP-IDF project
│   ├── main/           # display/touch drivers, Wi-Fi, portal, metrics, console
│   ├── components/     # on-board ST7796 / FT6336 drivers
│   ├── tools/          # capture.py, host_agent.py
│   ├── docs/DEBUGGING.md
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── docs/images/        # README screenshots
├── render.sh           # one-shot simulator render
└── LICENSE
```

`ui/` is compiled by both the firmware (`firmware/main/CMakeLists.txt` references `../../ui`) and the PC simulator, so what you see on the desktop is exactly what runs on the board.

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

- A full SPI refresh costs ~60 ms; the UI is designed for partial redraws with 1–2 FPS data updates and restrained animation
- Without the host agent, power/temperature/GPU/RAM and host uptime show "no data"
- The bundled CJK font covers 415 common characters (`ui/src/font_cjk_16.c`); rare glyphs will be missing
- `sim/` uses LVGL 9.3 while the firmware uses LVGL 9.6; the APIs are compatible but the simulator prints a few deprecation warnings

## License

[MIT](LICENSE)
