<p align="left"><img alt="OSPTEK" src="../../images/logo.png" width="200" /></p>

<h1 align="center">ESP32-P4 · CO6300 MIPI DSI + CST3530 + LVGL 9 Example</h1>

<p align="center"><b>Bring-up example for the 3.19″ 262×928 AMOLED (AM319M262928ZS)</b></p>

<p align="center">English | <a href="./README.md">简体中文</a> · <a href="../../README_EN.md">Back to product page</a></p>

## Contents

- [What it does](#what-it-does)
- [Wiring](#wiring)
- [Requirements](#requirements)
- [Build and flash](#build-and-flash)
- [Project layout](#project-layout)
- [Configuration](#configuration)
- [Version compatibility](#version-compatibility)
- [Migration notes](#migration-notes)
- [Troubleshooting](#troubleshooting)

---

## What it does

On an ESP32-P4 the example runs through the following steps and ends in LVGL 9's `lv_demo_widgets()`:

1. Powers the AMOLED through `VCI_EN` (held low for 60 ms, then high, then a 50 ms wait);
2. Brings up I2C (400 kHz), scans the bus and creates the CST3530 touch driver — deliberately before DSI: seeing `0x58` proves the flex cable, 3V3 and the touch reset are fine;
3. Enables the MIPI DSI PHY LDO (channel 3, 2.5 V) and creates a 1-data-lane / 360 Mbps DSI bus;
4. Hardware-resets the panel, reads its ID (`RDDID 04h`, `33 12 00` for this module), sends the CO6300 init sequence over the DBI IO (`lcd_init_cmds[]` in `main.c`, matching [`docs/AM319M262928ZS_CO6300_init_BOE3.19.txt`](../../docs/AM319M262928ZS_CO6300_init_BOE3.19.txt)), creates an RGB565 DPI panel and enables DMA2D;
5. Registers display and touch with `esp_lvgl_port` and starts the LVGL demo.

## Wiring

The module's own 30-pin board-to-board connector cannot be plugged into a dev board directly; the OSPTEK adapter board (schematic below) is required. On the adapter, **J1** is a 15-pin flex connector with the Raspberry Pi DSI pinout and **J2** is a 2×3 header (LCD_RST, VCI_EN, TP_RST, TP_INT, LCD_TE, GND).

### 15-way flex cable: adapter J1 ↔ dev board MIPI-DSI connector

| J1 pin | Signal | Notes |
| ------ | ------ | ----- |
| 1 / 4 / 7 / 10 / 13 | GND | |
| 2 / 3 | DSI D1 N / P | D1 is not bonded out on the module; unused |
| 5 / 6 | DSI CLK N / P | |
| 8 / 9 | DSI D0 N / P | 1 data lane, 360 Mbps; PHY powered by LDO channel 3 at 2.5 V |
| 11 / 12 | I2C SCL / SDA (touch) | GPIO8 / GPIO7 on the dev board, I2C0, internal pull-up, 400 kHz |
| 14 / 15 | +3V3 | The only supply for the whole panel, touch included |

Pin 1 (GND) must line up at both ends of the cable — a reversed cable shorts 3V3 to GND. Check with a meter across the adapter's C1 before powering up.

### J2 control lines: jumper wires → GPIO

| J2 signal | Required? | Default GPIO | Notes |
| --------- | --------- | ------------ | ----- |
| LCD_RST | **yes** | 6 | Active-low reset. -1 switches to a DCS software reset, but the adapter's RC reset alone is not enough — see below |
| VCI_EN | **yes** | 22 | AMOLED PMIC enable; the example holds it low for 60 ms at boot, then high |
| GND | **yes** | — | |
| TP_RST | optional | -1 (not wired) | Released by the adapter's 10K + 0.1 µF RC when not wired |
| TP_INT | optional | -1 (not wired) | `esp_lvgl_port` polls when unset; **as soon as a pin is configured it switches to interrupt (event) mode, so an unwired INT means touch never responds** |
| LCD_TE | no | — | Not used in video mode |

Pins are set in `idf.py menuconfig` → *Example Configuration (AM319M262928ZS bring-up)* (`CONFIG_EXAMPLE_PIN_NUM_LCD_RST` / `_LCD_VCI_EN` / `_TOUCH_SDA` / `_TOUCH_SCL` / `_TOUCH_RST` / `_TOUCH_INT`; -1 means the line is not wired).

**The flex cable alone does not light the panel** (measured 2026-08-29): the adapter pulls VCI_EN and LCD_RST high, but its RC releases RESX about 1 ms after 3V3 rises, before the PMIC has brought VCI up (the CO6300 datasheet, §5.8.2, wants RESX released ≥10 ms after VCI is valid). The panel then stops answering: the DSI ID read never returns and a DCS software reset does not recover it. With LCD_RST + VCI_EN + GND wired the ID reads back as `LCD ID: 33 12 00` and the display comes up.

### Header positions on specific dev boards

| Signal | ESP32-P4 module base board V1.3 (docs in [`docs/boards/`](../../../../docs/boards/), same for P4C5 / P4C6 core boards) | Board in the adapter demo photo |
| ------ | ---- | ---- |
| LCD_RST | GPIO6 → J4 pin 23 | GPIO6 |
| VCI_EN | GPIO22 → J4 pin 33 | GPIO22 |
| GND | J4 pin 1 / 2 | GND |
| TP_RST (optional) | GPIO23 → J4 pin 34, set 23 in menuconfig | GPIO16 |
| TP_INT (optional) | GPIO21 → J4 pin 32, set 21 in menuconfig | GPIO17 |
| MIPI-DSI connector | The 15-way connector silk-screened **MIPI-DSI** (the MIPI-CSI one next to it is not it) | 15-way DSI connector |

The committed `sdkconfig` is set up for "V1.3 base board with the TP lines wired" (TP_RST 23, TP_INT 21); set both to -1 when the TP lines are not wired, or 16 / 17 for the board in the demo photo. Regenerating `sdkconfig` falls back to the Kconfig defaults (-1 / -1, polling).

Notes for the V1.3 base board: GPIO14–19 form the SDIO bus between the P4 and the wireless co-processor (C5 / C6) and are not on the headers, so TP_RST / TP_INT must **not** be set to 16 / 17 (INT never fires, so touch stays dead); the touch I2C bus also carries the base board's ES8311 audio codec (`0x18`), so seeing it in the scan is normal; GPIO20 enables the SY7200 backlight boost, which an AMOLED does not need — leave it alone.

DPI timing: 262 × 928, HSYNC 4 / HBP 32 / HFP 32, VSYNC 4 / VBP 8 / VFP 8, 16 MHz pixel clock (about 51 Hz).

The module requires an ESP32-P4 with PSRAM (the example is configured for 16 MB flash and 200 MHz PSRAM). The adapter board schematic is in [`docs/3.19寸262x928 AMOLED转接板.pdf`](../../docs/3.19%E5%AF%B8262x928%20AMOLED%E8%BD%AC%E6%8E%A5%E6%9D%BF.pdf); a photo of the bring-up setup is in [`assets/adapter-board-demo.jpg`](../../assets/adapter-board-demo.jpg).

## Requirements

| Item | Requirement |
| ---- | ----------- |
| ESP-IDF | **v6.1** (verified). Accepted range: v5.5.5+, v6.0.3+, v6.1+ — see [Version compatibility](#version-compatibility) |
| Target | ESP32-P4 |
| Components | `espressif/esp_lvgl_port ^2.9.0`, `lvgl/lvgl 9.5.*`, `espressif/esp_lcd_touch ^1.2.1`. Downloaded into `managed_components/` from the [ESP Component Registry](https://components.espressif.com/) on the first build (network access required) |

Installing ESP-IDF v6.1 (macOS / Linux):

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1
cd esp-idf-v6.1 && ./install.sh esp32p4
```

On Windows use the [ESP-IDF installer](https://dl.espressif.com/dl/esp-idf/) or pick v6.1 in the VS Code ESP-IDF extension.

## Build and flash

```bash
# 1. Activate the ESP-IDF environment (once per terminal)
. ~/esp/esp-idf-v6.1/export.sh

# 2. Enter the example directory
cd versions/AM319M262928ZS/examples/esp32p4-3.19-amoled-262x928-mipi-co6300-bringup

# 3. Build
idf.py build

# 4. Flash and open the serial monitor (replace PORT, e.g. /dev/cu.usbmodem1101, /dev/ttyACM0, COM5)
idf.py -p PORT flash monitor
```

- A `sdkconfig` targeting esp32p4 ships with the repository, so `idf.py set-target` is not needed. If you delete it, `idf.py set-target esp32p4` regenerates it from `sdkconfig.defaults`.
- Building with a different IDF release migrates and rewrites `sdkconfig`; that is expected. Run `git checkout sdkconfig` if you do not want to commit the churn.
- After switching IDF versions or editing `idf_component.yml`, run `idf.py fullclean` before rebuilding.
- Leave the monitor with `Ctrl+]`.

VS Code: install the ESP-IDF extension, select v6.1, open this example folder and use Build / Flash / Monitor from the status bar.

After flashing, the serial log should include:

```text
I (xxx) Main: Initialize I2C bus
I (xxx) Main: Scanning I2C bus...
I (xxx) Main: Found I2C device at address 0x58
I (xxx) Main: Initialize touch controller
I (xxx) Main: MIPI DSI PHY Powered on
I (xxx) co6300_mipi: LCD ID: 33 12 00
I (xxx) LVGL: Starting LVGL task
```

`LCD ID: 33 12 00` is the ID code programmed by the vendor init sequence (`FE 40 / D8 33 / D9 12 / DA 00`). If the log stops after `esp_lcd_new_panel_io_dbi!` and `task_wdt` messages follow, the panel is not answering DSI reads — see [Troubleshooting](#troubleshooting).

## Project layout

```text
esp32p4-3.19-amoled-262x928-mipi-co6300-bringup/
├── CMakeLists.txt
├── main/
│   ├── main.c                  # power sequence, DSI/DPI, touch and LVGL init
│   ├── idf_component.yml       # component dependencies (esp_lvgl_port / lvgl / idf range)
│   └── CMakeLists.txt
├── components/
│   ├── esp_lcd_co6300/         # CO6300 panel driver (MIPI DSI / SPI / QSPI)
│   └── esp_lcd_touch_cst3530/  # CST3530 touch driver (esp_lcd_touch interface)
├── partitions.csv              # nvs / phy_init / factory 8 MB / storage 7 MB (spiffs)
├── sdkconfig.defaults          # minimal configuration
├── sdkconfig                   # full configuration (generated by IDF, tracked in git)
└── dependencies.lock           # component manager lock file
```

`managed_components/` and `build/` are created on the first build.

## Configuration

Key entries in `sdkconfig.defaults`:

| Option | Value | Notes |
| ------ | ----- | ----- |
| `CONFIG_IDF_TARGET` | `esp32p4` | |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` / `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | 16 MB QIO flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | Uses `partitions.csv` |
| `CONFIG_SPIRAM` / `CONFIG_SPIRAM_SPEED_200M` / `CONFIG_SPIRAM_XIP_FROM_PSRAM` | y | 200 MHz PSRAM with code execution from PSRAM |
| `CONFIG_IDF_EXPERIMENTAL_FEATURES` | y | Required for 200 MHz PSRAM |
| `CONFIG_CACHE_L2_CACHE_256KB` / `CONFIG_CACHE_L2_CACHE_LINE_128B` | y | |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | y | `-O2` |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 10240 | |
| `CONFIG_FREERTOS_HZ` | 1000 | |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | n | LVGL runs from PSRAM (XIP, 200 MHz). LVGL 9.5 marks about 117 KB of software blend code as fast-mem, while the executable SRAM region of the ESP32-P4 (`sram_low`) is only about 175 KB; Espressif's own P4 LVGL examples leave this option off as well |
| `CONFIG_LV_FONT_MONTSERRAT_8` … `_44` | y | Fonts used by the demos |
| `CONFIG_LV_USE_DEMO_BENCHMARK` / `_STRESS` / `_MUSIC` | y | Switch between `lv_demo_widgets()` / `lv_demo_music()` in `app_main()` |

Pins are configured in the menuconfig menu *Example Configuration (AM319M262928ZS bring-up)* (`CONFIG_EXAMPLE_PIN_NUM_*`, see [Wiring](#wiring)); the macros at the top of `main.c` set the DSI lane rate, the DPI timing and the LVGL draw-buffer height (`LCD_DRAW_BUFF_HEIGHT`, 120 lines by default, double-buffered).

## Version compatibility

| ESP-IDF | esp_lvgl_port | LVGL | Result |
| ------- | ------------- | ---- | ------ |
| v6.1 | 2.9.0 | 9.5.0 | ✅ Verified (default configuration) |
| v5.5.5, v6.0.3+ | 2.9.0 | 9.5.* | Expected to work (these releases carry the DPI `on_frame_buf_complete` callback); not tested |
| v6.0.0 – v6.0.2, v5.5.0 – v5.5.4 | 2.9.0 | – | ❌ No DPI `on_frame_buf_complete`; esp_lvgl_port 2.9.0 fails to compile. The idf range in `idf_component.yml` rejects these releases up front with `Version solving failed` |
| v6.0.2, v5.5.3 | 2.8.0~1 | 9.3.0 | ✅ Verified (with the change below) |

To build on v6.0.0 – v6.0.2 or v5.5.x, edit `main/idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lvgl_port: "~2.8.0"
  idf:
    version: '>=5.4.0'
```

then delete `dependencies.lock` and run `idf.py fullclean && idf.py build`. That combination was verified with LVGL 9.3.0; esp_lvgl_port 2.8.0 accepts LVGL `<10`, but 9.5 was not tested with it.

Moving from LVGL 9.3 to 9.5 needed exactly one change: turning off `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` (see [Configuration](#configuration)); the LVGL APIs used by `main.c` are unchanged.

The example sources themselves compile on both IDF 5.x and 6.x; every change is listed in [Migration notes](#migration-notes).

## Migration notes

The example originally targeted **ESP-IDF 5.5.x + esp_lvgl_port 2.7.0 + LVGL 9.3.0** with the legacy I2C driver for touch. Everything below is what it took to move it to **ESP-IDF 6.1 + esp_lvgl_port 2.9.0 + LVGL 9.5.0**; use it as a checklist for migrating your own project that started from the older example.

### ESP-IDF 5.5 → 6.x API changes

| Where | Before | After | Notes |
| ----- | ------ | ----- | ----- |
| `esp_lcd_co6300_mipi.c` | `panel_dev_config->color_space` | `panel_dev_config->rgb_ele_order` | 6.0 removed `color_space` / `rgb_endian`; `rgb_ele_order` already exists on 5.x |
| `main.c` DPI config | `.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565` | `.in_color_format = LCD_COLOR_FMT_RGB565` | 6.0 removed `pixel_format`; `in_color_format` is available since 5.4 |
| `main.c` DPI config | `.flags.use_dma2d = true` | `esp_lcd_dpi_panel_enable_dma2d(panel)` after creating the panel | 6.0 removed the flag; the example keeps the 5.x form behind `#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)` |
| `main.c` touch I2C | `driver/i2c.h`, `i2c_param_config()` + `i2c_driver_install()`, `esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0, …)` | `driver/i2c_master.h`, `i2c_new_master_bus()`, `esp_lcd_new_panel_io_i2c(bus_handle, …)` | Since 6.0 the LCD I2C IO layer only accepts an `i2c_master_bus_handle_t`; the legacy driver is EOL and removed in 7.0 |
| `main.c` I2C scan | `i2c_cmd_link_create()` / `i2c_master_cmd_begin()` | `i2c_master_probe(bus, addr, timeout)` | |
| `esp_lcd_touch_cst3530.c` | Direct legacy `i2c_master_*` transfers | `esp_lcd_panel_io_tx_param(io, -1, addr, 4)` to set the register address, `esp_lcd_panel_io_rx_param(io, -1, buf, len)` to read | 32-bit register addresses are sent as parameter bytes (`lcd_cmd = -1` means "no command"), which decouples the driver from the bus implementation; the STOP/START between write and read follows the CST3530 datasheet |
| `esp_lcd_touch_cst3530.h` | `CST3530_I2C_NUM` macro | Removed; `ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()` gains `.scl_speed_hz = 400 * 1000` | The new I2C driver requires `scl_speed_hz > 0` |
| `esp_lcd_co6300.h` | `CO6300_262_928_PANEL_60HZ_DPI_CONFIG(px_format)` used `.pixel_format` and `.flags.use_dma2d` | Parameter is now a `lcd_color_format_t`; `use_dma2d` is added conditionally per IDF version | |

### Component versions

| Component | Before | After | Notes |
| --------- | ------ | ----- | ----- |
| `espressif/esp_lvgl_port` | `^2.7.0` (2.7.0) | `^2.9.0` (2.9.0) | 2.7.1+ fixes the IDF 6 PPA colour types; 2.9.0 needs IDF v5.5.5 / v6.0.3+ / v6.1+ |
| `lvgl/lvgl` | `9.3.*` (9.3.0) | `9.5.*` (9.5.0) | No changes needed in `main.c` |
| `idf` | `>=5.1.0` | `>=5.5.5,!=6.0.0,!=6.0.1,!=6.0.2` | Excludes releases without the DPI `on_frame_buf_complete` callback |
| `espressif/esp_lcd_touch` | `^1.2.1` | unchanged | |

### sdkconfig

| Option | Before | After | Notes |
| ------ | ------ | ----- | ----- |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | y | n | LVGL 9.5 marks about 117 KB of code as fast-mem, more than the ~175 KB of executable SRAM on the ESP32-P4 |

Other options are migrated by IDF 6.x automatically (for example `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT` → `CONFIG_ESP_SYSTEM_MEMPROT`) and need no manual work.

### Migration steps

1. Apply the API changes from the table above;
2. Update the version ranges in `main/idf_component.yml`, delete `dependencies.lock` and `managed_components/`;
3. Turn off `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` in `sdkconfig` / `sdkconfig.defaults`;
4. Activate the ESP-IDF v6.1 environment and run `idf.py fullclean && idf.py build`.

## Troubleshooting

| Symptom | What to check |
| ------- | ------------- |
| `ERROR: Version solving failed ... idf` | The active IDF release is outside the supported range — see [Version compatibility](#version-compatibility) |
| Component download fails on the first build | `components.espressif.com` must be reachable; rerun `idf.py reconfigure` once the network is back |
| The log stops after `esp_lcd_new_panel_io_dbi!`, then `task_wdt: ... CPU 0: main` repeats every 5 s | The panel is not answering the DSI ID read and IDF's `mipi_dsi_hal_host_gen_read_short_packet` waits for the read FIFO without a timeout, so the main task spins forever. Almost always power / reset sequencing: LCD_RST and VCI_EN not wired (the adapter's RC reset alone is not enough), or the flex cable reversed / not seated. Wire the three lines (see [Wiring](#wiring)) and retry |
| `read ID failed`, or `LCD ID` is not `33 12 00` | Check the VCI_EN and LCD RST wiring and the pin numbers in menuconfig; confirm the adapter board is powered |
| The I2C scan does not find `0x58` | Check the flex cable (pins 11 / 12 carry the touch I2C) and 3V3; if TP_RST is wired, check its pin setting; the CST3530 only answers after RST is released |
| The I2C scan finds `0x18` | The base board's ES8311 audio codec shares the touch I2C bus — expected |
| Display works but touch never responds | `CONFIG_EXAMPLE_PIN_NUM_TOUCH_INT` names a pin that is not wired (or wired elsewhere): with an INT pin configured, `esp_lvgl_port` switches to event mode and only reads the touch controller on interrupts. Set it to -1 to poll |
| `gpio: conflict found for GPIO[n]` (n = the TP_RST pin) | `app_touch_init()` releases TP_RST by hand and the touch driver then runs `gpio_config` on the same pin again; IDF 6 flags the double configuration. Harmless |
| Display works but touch is mirrored / rotated | Adjust `swap_xy` / `mirror_x` / `mirror_y` in `tp_cfg.flags` inside `app_touch_init()` |
| Linking prints many `--enable-non-contiguous-regions discards section` errors and ends with `ld terminated with signal 11` | IRAM overflow, usually because `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` was enabled: LVGL 9.5 places about 117 KB of blend code in IRAM while the P4's executable SRAM is only about 175 KB. Keep the option off; if you really need IRAM placement, first disable unused `CONFIG_LV_DRAW_SW_SUPPORT_*` colour formats in the LVGL configuration |

---

<p align="center"><sub>© 2026 OSPTEK · Licensed under CC BY 4.0</sub></p>
