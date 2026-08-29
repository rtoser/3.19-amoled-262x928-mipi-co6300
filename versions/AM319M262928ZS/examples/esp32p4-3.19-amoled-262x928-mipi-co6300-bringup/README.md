<p align="left"><img alt="OSPTEK" src="../../images/logo.png" width="200" /></p>

<h1 align="center">ESP32-P4 · CO6300 MIPI DSI + CST3530 + LVGL 9 示例</h1>

<p align="center"><b>3.19″ 262×928 AMOLED（AM319M262928ZS）点亮示例</b></p>

<p align="center"><a href="./README_EN.md">English</a> | 简体中文 · <a href="../../README.md">返回产品页</a></p>

## 目录

- [功能说明](#功能说明)
- [硬件连接](#硬件连接)
- [环境要求](#环境要求)
- [编译与烧录](#编译与烧录)
- [工程结构](#工程结构)
- [配置说明](#配置说明)
- [渲染模式](#渲染模式)
- [触摸驱动的健壮性](#触摸驱动的健壮性)
- [版本兼容性](#版本兼容性)
- [迁移说明](#迁移说明)
- [常见问题](#常见问题)

---

## 功能说明

本示例在 ESP32-P4 上完成以下流程，最终运行 LVGL 9 的 `lv_demo_widgets()`：

1. 通过 `VCI_EN` 给 AMOLED 上电（先拉低 60 ms 再拉高，等待 50 ms）；
2. 初始化 I2C（400 kHz）并扫描总线，创建 CST3530 触摸驱动——放在 DSI 之前：能扫到 `0x58` 就说明排线、3V3 与触摸复位都正常；
3. 打开 MIPI DSI PHY 的 LDO（通道 3，2.5 V），创建 1 data lane / 360 Mbps 的 DSI 总线；
4. 硬件复位面板、读取 ID（`RDDID 04h`，本模组为 `33 12 00`），通过 DBI IO 发送 CO6300 初始化序列（`main.c` 中的 `lcd_init_cmds[]`，对应 [`docs/AM319M262928ZS_CO6300_init_BOE3.19.txt`](../../docs/AM319M262928ZS_CO6300_init_BOE3.19.txt)），创建 RGB565 DPI 面板并启用 DMA2D；
5. 通过 `esp_lvgl_port` 注册显示与触摸（VSYNC 同步的 direct mode，见[渲染模式](#渲染模式)），启动 LVGL 演示。

## 硬件连接

模组自带的 30-pin 板对板座不能直插开发板，必须经过 OSPTEK 转接板（原理图见下）。转接板 **J1** 是树莓派 15-pin DSI 定义的排线座，**J2** 是 2×3 排针（LCD_RST、VCI_EN、TP_RST、TP_INT、LCD_TE、GND）。

### 15P 排线：转接板 J1 ↔ 开发板 MIPI-DSI 座

| J1 脚 | 信号 | 说明 |
| ----- | ---- | ---- |
| 1 / 4 / 7 / 10 / 13 | GND | |
| 2 / 3 | DSI D1 N / P | 模组未引出 D1，闲置 |
| 5 / 6 | DSI CLK N / P | |
| 8 / 9 | DSI D0 N / P | 1 data lane，360 Mbps；PHY 由 LDO 通道 3 供电 2.5 V |
| 11 / 12 | I2C SCL / SDA（触摸） | 开发板侧为 GPIO8 / GPIO7，I2C0，内部上拉，400 kHz |
| 14 / 15 | +3V3 | 整块屏（含触摸）的唯一供电 |

排线两端的 pin 1（GND）必须对齐——反插会把 3V3 短到 GND；上电前先用万用表量转接板 C1 两端不短路。

### J2 控制线：杜邦线 → GPIO

| J2 信号 | 必要性 | 默认 GPIO | 说明 |
| ------- | ------ | --------- | ---- |
| LCD_RST | **必接** | 6 | 低电平复位。设为 -1 时改用 DCS 软件复位，但只靠转接板的 RC 复位不够，见下 |
| VCI_EN | **必接** | 22 | AMOLED PMIC 使能；示例上电时先拉低 60 ms 再拉高 |
| GND | **必接** | — | |
| TP_RST | 可选 | -1（不接） | 不接时由转接板 10K + 0.1 µF 的 RC 自动释放 |
| TP_INT | 可选 | -1（不接） | 不接时 `esp_lvgl_port` 轮询读取；**一旦配置了引脚就切到中断（事件）模式，该线没接则触摸完全无响应** |
| LCD_TE | 不用 | — | 视频模式用不到 |

引脚在 `idf.py menuconfig` → *Example Configuration (AM319M262928ZS bring-up)* 中修改（`CONFIG_EXAMPLE_PIN_NUM_LCD_RST` / `_LCD_VCI_EN` / `_TOUCH_SDA` / `_TOUCH_SCL` / `_TOUCH_RST` / `_TOUCH_INT`，-1 表示该线未连接）。

**只接排线点不亮**（2026-08-29 实测）：转接板虽然把 VCI_EN、LCD_RST 都上拉了，但 RC 在 3V3 上电后约 1 ms 就释放 RESX，早于 PMIC 把 VCI 拉稳（CO6300 手册 §5.8.2 要求 VCI 有效后 ≥10 ms 再释放复位），面板进入不应答状态——DSI 读 ID 无回包、DCS 软件复位也救不回来。接上 LCD_RST + VCI_EN + GND 三根线后读回 `LCD ID: 33 12 00`，屏幕点亮。

### 各开发板的排针位置

| 信号 | ESP32-P4 模组基础底板 V1.3（资料见 [`docs/boards/`](../../../../docs/boards/)，P4C5 / P4C6 核心板通用） | 转接板实拍所用开发板 |
| ---- | ---- | ---- |
| LCD_RST | GPIO6 → J4 pin 23 | GPIO6 |
| VCI_EN | GPIO22 → J4 pin 33 | GPIO22 |
| GND | J4 pin 1 / 2 | GND |
| TP_RST（可选） | GPIO23 → J4 pin 34，menuconfig 设 23 | GPIO16 |
| TP_INT（可选） | GPIO21 → J4 pin 32，menuconfig 设 21 | GPIO17 |
| MIPI-DSI 座 | 丝印 **MIPI-DSI** 的 15P 座（旁边的 MIPI-CSI 别插错） | 15P DSI 座 |

仓库自带的 `sdkconfig` 已按「V1.3 底板 + 接了 TP 线」设置（TP_RST 23、TP_INT 21）；没接 TP 线就把这两项设为 -1，实拍板设为 16 / 17。删除 `sdkconfig` 重新生成时会回到 Kconfig 默认值（-1 / -1，轮询）。

V1.3 底板注意：GPIO14–19 是 P4 与无线协处理器（C5 / C6）之间的 SDIO 总线，排针没有引出，**不能**把 TP_RST / TP_INT 配成 16 / 17（INT 永远不会触发，触摸无响应）；触摸 I2C 总线上还挂着底板的 ES8311 音频编解码（`0x18`），扫描到它是正常的；GPIO20 是 SY7200 背光升压的使能，AMOLED 没有背光，不要拉高。

DPI 时序：262 × 928，HSYNC 4 / HBP 32 / HFP 32，VSYNC 4 / VBP 8 / VFP 70，像素时钟 20 MHz（PLL_F240M ÷ 12），刷新率 20 000 000 ÷ (330 × 1010) = **60.01 Hz**，落在规格书 F_frm 25 ℃ 58.2–61.8 Hz 的窗口内。早期版本用 16 MHz / VFP 8 只有 51 Hz，低于规格下限。LVGL 刷新周期 `CONFIG_LV_DEF_REFR_PERIOD=16` ms，屏幕右下角由 `CONFIG_LV_USE_PERF_MONITOR` 叠加 FPS / CPU 指示。

主控需带 PSRAM（示例按 16 MB Flash、PSRAM 200 MHz 配置）。转接板原理图见 [`docs/3.19寸262x928 AMOLED转接板.pdf`](../../docs/3.19%E5%AF%B8262x928%20AMOLED%E8%BD%AC%E6%8E%A5%E6%9D%BF.pdf)，联调实拍见 [`assets/adapter-board-demo.jpg`](../../assets/adapter-board-demo.jpg)。

## 环境要求

| 项目 | 要求 |
| ---- | ---- |
| ESP-IDF | **v6.1**（已验证）。可用范围为 v5.5.5+、v6.0.3+、v6.1+，详见[版本兼容性](#版本兼容性) |
| 目标芯片 | ESP32-P4 |
| 组件依赖 | `espressif/esp_lvgl_port ^2.9.0`、`lvgl/lvgl 9.5.*`、`espressif/esp_lcd_touch ^1.2.1`。首次编译时自动从 [ESP Component Registry](https://components.espressif.com/) 下载到 `managed_components/`，需要联网 |

安装 ESP-IDF v6.1（macOS / Linux）：

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1
cd esp-idf-v6.1 && ./install.sh esp32p4
```

Windows 请使用 [ESP-IDF 安装器](https://dl.espressif.com/dl/esp-idf/) 或在 VS Code 的 ESP-IDF 扩展中选择 v6.1。

## 编译与烧录

```bash
# 1. 激活 ESP-IDF 环境（每个新终端都要执行）
. ~/esp/esp-idf-v6.1/export.sh

# 2. 进入示例目录
cd versions/AM319M262928ZS/examples/esp32p4-3.19-amoled-262x928-mipi-co6300-bringup

# 3. 编译
idf.py build

# 4. 烧录并打开串口监视器（PORT 换成实际串口，如 /dev/cu.usbmodem1101、/dev/ttyACM0、COM5）
idf.py -p PORT flash monitor
```

- 仓库已包含 `sdkconfig`（目标 esp32p4），无需 `idf.py set-target`。若删除了 `sdkconfig`，运行 `idf.py set-target esp32p4` 会按 `sdkconfig.defaults` 重新生成。
- 用不同版本的 IDF 编译时，IDF 会自动迁移并改写 `sdkconfig`，属正常现象；不想提交这些差异可执行 `git checkout sdkconfig`。
- 切换 IDF 版本或修改 `idf_component.yml` 后，建议 `idf.py fullclean` 后重新编译。
- 退出串口监视器：`Ctrl+]`。

VS Code：安装 ESP-IDF 扩展并选择 v6.1，用「打开文件夹」打开本示例目录，然后使用底部状态栏的 Build / Flash / Monitor。

烧录成功后串口日志应包含：

```text
I (xxx) Main: Initialize I2C bus
I (xxx) Main: Scanning I2C bus...
I (xxx) Main: Found I2C device at address 0x58
I (xxx) Main: Initialize touch controller
I (xxx) Main: MIPI DSI PHY Powered on
I (xxx) co6300_mipi: LCD ID: 33 12 00
I (xxx) LVGL: Starting LVGL task
```

`LCD ID: 33 12 00` 是厂商初始化序列写入的 ID code（`FE 40 / D8 33 / D9 12 / DA 00`）。日志停在 `esp_lcd_new_panel_io_dbi!` 之后、随后反复出现 `task_wdt`，说明面板没有应答 DSI 读，见[常见问题](#常见问题)。

## 工程结构

```text
esp32p4-3.19-amoled-262x928-mipi-co6300-bringup/
├── CMakeLists.txt
├── main/
│   ├── main.c                  # 上电时序、DSI/DPI、触摸、LVGL 初始化
│   ├── idf_component.yml       # 组件依赖（esp_lvgl_port / lvgl / idf 版本约束）
│   └── CMakeLists.txt
├── components/
│   ├── esp_lcd_co6300/         # CO6300 面板驱动（MIPI DSI / SPI / QSPI）
│   └── esp_lcd_touch_cst3530/  # CST3530 触摸驱动（esp_lcd_touch 接口）
├── partitions.csv              # nvs / phy_init / factory 8 MB / storage 7 MB (spiffs)
├── sdkconfig.defaults          # 最小配置
├── sdkconfig                   # 完整配置（由 IDF 生成并纳入版本管理）
└── dependencies.lock           # 组件管理器锁文件
```

`managed_components/` 与 `build/` 在首次编译时生成。

## 配置说明

`sdkconfig.defaults` 中的关键项：

| 配置 | 值 | 说明 |
| ---- | -- | ---- |
| `CONFIG_IDF_TARGET` | `esp32p4` | |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` / `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | 16 MB QIO Flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | 使用 `partitions.csv` |
| `CONFIG_SPIRAM` / `CONFIG_SPIRAM_SPEED_200M` / `CONFIG_SPIRAM_XIP_FROM_PSRAM` | y | PSRAM 200 MHz，并从 PSRAM 执行代码 |
| `CONFIG_IDF_EXPERIMENTAL_FEATURES` | y | PSRAM 200 MHz 需要 |
| `CONFIG_CACHE_L2_CACHE_256KB` / `CONFIG_CACHE_L2_CACHE_LINE_128B` | y | |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | y | `-O2` |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 10240 | |
| `CONFIG_FREERTOS_HZ` | 1000 | |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | n | LVGL 代码从 PSRAM（XIP，200 MHz）执行。LVGL 9.5 把约 117 KB 软件混合函数标记为 fast-mem，而 ESP32-P4 可执行的 SRAM 区（`sram_low`）只有约 175 KB，放不下；Espressif 官方 P4 LVGL 示例同样不启用此项 |
| `CONFIG_LV_FONT_MONTSERRAT_8` … `_44` | y | 演示所需字体 |
| `CONFIG_LV_USE_DEMO_BENCHMARK` / `_STRESS` / `_MUSIC` | y | 可在 `app_main()` 中切换 `lv_demo_widgets()` / `lv_demo_music()` |

引脚由 menuconfig 的 *Example Configuration (AM319M262928ZS bring-up)* 菜单配置（`CONFIG_EXAMPLE_PIN_NUM_*`，见[硬件连接](#硬件连接)）；`main.c` 顶部的宏可调整 DSI 速率与 DPI 时序。

## 渲染模式

示例使用 esp_lvgl_port 的 **VSYNC 同步 direct mode**：DPI 面板分配 2 幅 PSRAM 帧缓冲（`num_fbs = 2`，各 262 × 928 × 2 B = 486 KB），LVGL 直接在其中绘制（`buffer_size` 为整屏、`flags.direct_mode = 1`），`avoid_tearing = 1` 让 flush 等到 DSI 发完当前帧（`on_frame_buf_complete`）再切换帧缓冲。效果是画面永远是完整帧、刷新率锁定在面板的 60 Hz；代价是 LVGL 任务在等 VSYNC 时阻塞（叠加层里的 CPU% 会把这段等待算成忙碌，真实开销看渲染时间），以及一帧渲染超过 16.7 ms 时掉到 30 FPS（VSYNC 减半）。

2026-08-29 在 V1.3 底板上用 `lv_demo_benchmark`（`CONFIG_LV_USE_DEMO_BENCHMARK`，在 `app_main()` 里替换 `lv_demo_widgets()` 即可运行）对比两种模式（均为 60 Hz 时序、16 ms 刷新周期、1 ms tick）：

| 场景 | 局部刷新（120 行 SRAM 双缓冲 + DMA2D 拷贝，无 VSYNC） | **direct mode + VSYNC（当前默认）** |
| ---- | ---- | ---- |
| Empty / rectangles / images / arcs / labels / containers | 50 FPS（5 ms tick 量化所致） | **59–60 FPS** |
| Rotated ARGB images | 50 FPS，渲染 10 ms | 32 FPS，渲染 15 ms |
| Screen sized text | 25 FPS，渲染 37 ms | 30 FPS，渲染 20 ms |
| Containers with overlay | 46 FPS，渲染 19 ms | 30 FPS，渲染 22 ms |
| Containers with scrolling | 49 FPS，渲染 15 ms | 30 FPS，渲染 18 ms |
| Widgets demo | 29 FPS，渲染 21 ms | 22 FPS，渲染 20 ms |
| 全部场景平均 | 46 FPS | **49 FPS** |

直写 PSRAM 比内部 SRAM 慢，重场景的渲染时间多 3–5 ms；文字密集的场景是 CPU 瓶颈（字形混合全在 CPU 上，PPA 只加速不透明填充和图片）。想回到局部刷新模式：`num_fbs = 1`、`buffer_size` 改为 `MIPI_DSI_LCD_H_RES * 120`、去掉 `direct_mode`、`avoid_tearing = false`。

## 触摸驱动的健壮性

2026-08-29 在 V1.3 底板上快速拖动界面时复现了两种故障，根因相同——触摸 I2C 总线在高频读取下偶发 NACK：

- **卡死**：`esp_lcd_panel_io_i2c_config_t.transaction_timeout_ms` 未设置即为「永远等」，从机拉死总线时 `i2c_master_transmit` 无限阻塞，LVGL 任务停在触摸读取里（显示 DMA 仍在刷，所以画面定格而非黑屏）。
- **重启**：`esp_lcd_touch_read_data()` 返回错误时 esp_lvgl_port 用 `ESP_ERROR_CHECK` 直接 `abort()`。

`components/esp_lcd_touch_cst3530` 现在的做法：

1. I2C 事务超时 20 ms（`ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()`）。
2. 读取失败不上抛：上报 0 个触点，记入 `esp_lcd_touch_cst3530_get_stats()` 的计数；连续 3 次失败后 `i2c_master_bus_reset()`（9 个 SCL 脉冲释放被拉死的 SDA）+ 触摸芯片复位，需要应用先调用 `esp_lcd_touch_cst3530_set_i2c_bus()` 注册总线。
3. 报文协议对齐海栅官方驱动（[viewesmart/esp_lcd_touch_cst3530](https://components.espressif.com/components/viewesmart/esp_lcd_touch_cst3530) 内嵌的 `hyn_cst66xx.c`）：只读 4 字节头 + 每指 5 字节而不是固定 64 字节（总线占用降到约 1/7），校验 16 位和（`0x55 + Σ数据`）并最多重试 2 次，忽略 `event == 0` 的抬起包，复位后写入 normal-mode 序列（`0xD0000400` ×2 关闭芯片内部低功耗 I2C 上拉，再 `0xD0000000 / 0xD0000C00 / 0xD0000100`），复位脉冲 10 ms + 200 ms（TPON）。

实测：加固后 135 s 连续拖动出现 10 次孤立 NACK，无连续失败、无卡死、无重启；再换用官方报文协议后，150 s 同样强度的拖动 **0 次 I2C 错误**。

## 版本兼容性

| ESP-IDF | esp_lvgl_port | LVGL | 结果 |
| ------- | ------------- | ---- | ---- |
| v6.1 | 2.9.0 | 9.5.0 | ✅ 已验证（默认配置） |
| v5.5.5、v6.0.3+ | 2.9.0 | 9.5.* | 应可用（这些版本含 DPI `on_frame_buf_complete` 回调），未实测 |
| v6.0.0 – v6.0.2、v5.5.0 – v5.5.4 | 2.9.0 | – | ❌ 缺少 DPI `on_frame_buf_complete`，esp_lvgl_port 2.9.0 内部编译失败；`idf_component.yml` 的 idf 版本约束会在依赖解析阶段直接报 `Version solving failed` |
| v6.0.2、v5.5.3 | 2.8.0~1 | 9.3.0 | ✅ 已验证（需按下述修改） |

如需在 v6.0.0 – v6.0.2 或 v5.5.x 上编译，修改 `main/idf_component.yml`：

```yaml
dependencies:
  espressif/esp_lvgl_port: "~2.8.0"
  idf:
    version: '>=5.4.0'
```

然后删除 `dependencies.lock`，执行 `idf.py fullclean && idf.py build`。该组合实测用的是 LVGL 9.3.0；esp_lvgl_port 2.8.0 允许 LVGL `<10`，搭配 9.5 未实测。

LVGL 从 9.3 升到 9.5 时唯一需要的改动是关闭 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`（见[配置说明](#配置说明)）；`main.c` 使用的 LVGL API 无变化。

示例代码本身同时兼容 IDF 5.x 与 6.x，具体改动见[迁移说明](#迁移说明)。

## 迁移说明

本示例最初基于 **ESP-IDF 5.5.x + esp_lvgl_port 2.7.0 + LVGL 9.3.0**，触摸走 legacy I2C 驱动。下面是迁移到 **ESP-IDF 6.1 + esp_lvgl_port 2.9.0 + LVGL 9.5.0** 所做的全部改动，也可作为把基于旧版示例的自有工程迁到 IDF 6.x 的对照表。

### ESP-IDF 5.5 → 6.x API 变更

| 位置 | 旧写法 | 新写法 | 说明 |
| ---- | ------ | ------ | ---- |
| `esp_lcd_co6300_mipi.c` | `panel_dev_config->color_space` | `panel_dev_config->rgb_ele_order` | 6.0 移除了 `color_space` / `rgb_endian`，`rgb_ele_order` 在 5.x 已存在 |
| `main.c` DPI 配置 | `.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565` | `.in_color_format = LCD_COLOR_FMT_RGB565` | 6.0 移除了 `pixel_format`，`in_color_format` 自 5.4 起可用 |
| `main.c` DPI 配置 | `.flags.use_dma2d = true` | 创建面板后调用 `esp_lcd_dpi_panel_enable_dma2d(panel)` | 6.0 移除了该 flag；示例用 `#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)` 同时保留 5.x 写法 |
| `main.c` 触摸 I2C | `driver/i2c.h`、`i2c_param_config()` + `i2c_driver_install()`，`esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0, …)` | `driver/i2c_master.h`、`i2c_new_master_bus()`，`esp_lcd_new_panel_io_i2c(bus_handle, …)` | 6.0 起 LCD 的 I2C IO 层只接受 `i2c_master_bus_handle_t`；legacy 驱动已 EOL，7.0 移除 |
| `main.c` I2C 扫描 | `i2c_cmd_link_create()` / `i2c_master_cmd_begin()` | `i2c_master_probe(bus, addr, timeout)` | |
| `esp_lcd_touch_cst3530.c` | 直接调用 legacy `i2c_master_*` 读写 | `esp_lcd_panel_io_tx_param(io, -1, addr, 4)` 写寄存器地址、`esp_lcd_panel_io_rx_param(io, -1, buf, len)` 读数据 | 32 位寄存器地址以参数字节发送（`lcd_cmd = -1` 表示不带命令），驱动与总线实现解耦；写、读之间的 STOP/START 符合 CST3530 手册 |
| `esp_lcd_touch_cst3530.h` | `CST3530_I2C_NUM` 宏 | 删除；`ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG()` 增加 `.scl_speed_hz = 400 * 1000` | 新 I2C 驱动要求 `scl_speed_hz > 0` |
| `esp_lcd_co6300.h` | `CO6300_262_928_PANEL_60HZ_DPI_CONFIG(px_format)` 使用 `.pixel_format` 与 `.flags.use_dma2d` | 参数改为 `lcd_color_format_t`，`use_dma2d` 按 IDF 版本条件加入 | |

### 组件版本

| 组件 | 旧 | 新 | 说明 |
| ---- | -- | -- | ---- |
| `espressif/esp_lvgl_port` | `^2.7.0`（2.7.0） | `^2.9.0`（2.9.0） | 2.7.1 起修复 IDF 6 的 PPA 颜色类型；2.9.0 需要 IDF v5.5.5 / v6.0.3+ / v6.1+ |
| `lvgl/lvgl` | `9.3.*`（9.3.0） | `9.5.*`（9.5.0） | `main.c` 无需改动 |
| `idf` | `>=5.1.0` | `>=5.5.5,!=6.0.0,!=6.0.1,!=6.0.2` | 排除缺少 DPI `on_frame_buf_complete` 的版本 |
| `espressif/esp_lcd_touch` | `^1.2.1` | 不变 | |

### sdkconfig

| 配置 | 旧 | 新 | 说明 |
| ---- | -- | -- | ---- |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | y | n | LVGL 9.5 标记为 fast-mem 的代码约 117 KB，超过 ESP32-P4 约 175 KB 的可执行 SRAM |

其余选项由 IDF 6.x 自动迁移（例如 `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT` → `CONFIG_ESP_SYSTEM_MEMPROT`），无需手动处理。

### 迁移步骤

1. 按上表修改 API 调用；
2. 更新 `main/idf_component.yml` 的版本约束，删除 `dependencies.lock` 与 `managed_components/`；
3. 在 `sdkconfig` / `sdkconfig.defaults` 中关闭 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`；
4. 激活 ESP-IDF v6.1 环境，执行 `idf.py fullclean && idf.py build`。

## 常见问题

| 现象 | 处理 |
| ---- | ---- |
| `ERROR: Version solving failed ... idf` | 当前 IDF 版本不在支持范围内，参见[版本兼容性](#版本兼容性) |
| 首次编译时下载组件失败 | 需要能访问 `components.espressif.com`；网络恢复后重新执行 `idf.py reconfigure` |
| 日志停在 `esp_lcd_new_panel_io_dbi!` 之后，随后每 5 s 一次 `task_wdt: ... CPU 0: main` | 面板对 DSI 读 ID 没有应答，而 IDF 的 `mipi_dsi_hal_host_gen_read_short_packet` 等待读 FIFO 的循环没有超时，主任务卡死。原因几乎都是上电 / 复位时序：LCD_RST、VCI_EN 没接线（只靠转接板 RC 复位不够）、排线反插或未插紧。接好三根线（见[硬件连接](#硬件连接)）再试 |
| 日志出现 `read ID failed`，或 `LCD ID` 不是 `33 12 00` | 检查 VCI_EN、LCD RST 接线与 menuconfig 里的引脚号，确认转接板供电 |
| I2C 扫描未发现 `0x58` | 检查排线（11 / 12 脚走触摸 I2C）与 3V3；若接了 TP_RST 线，核对其引脚配置；CST3530 需 RST 释放后才会应答 |
| I2C 扫描到 `0x18` | 底板上的 ES8311 音频编解码与触摸共用 I2C，正常 |
| 屏幕正常但触摸完全无响应 | `CONFIG_EXAMPLE_PIN_NUM_TOUCH_INT` 配了引脚但该线没接（或接错）：`esp_lvgl_port` 在配置了 INT 时改为事件模式，只在中断到来时读触摸。不接 INT 就设为 -1 改回轮询 |
| `gpio: conflict found for GPIO[n]`（n 为 TP_RST 脚） | 示例先在 `app_touch_init()` 里手动释放 TP_RST，触摸驱动创建时又对同一脚 `gpio_config` 一次，IDF 6 对重复配置的提示，无害 |
| 屏幕正常但触摸方向不对 | 调整 `app_touch_init()` 中 `tp_cfg.flags` 的 `swap_xy` / `mirror_x` / `mirror_y` |
| 日志偶尔出现 `CST3530: report read failed (ESP_ERR_INVALID_RESPONSE …)` | 触摸 I2C 单次失败，驱动按「无触摸」处理并继续；连续 3 次才会做一次总线复位 + 触摸芯片复位（日志 `recovery #n`，约 210 ms）。频繁出现请检查排线 11/12 脚与上拉；本模组的触摸 I2C 为 1.8 V 电平而转接板/底板上拉到 3.3 V，属于硬件层的已知风险 |
| 拖动界面后触摸失效 / 整机重启（旧版本） | 见[触摸驱动的健壮性](#触摸驱动的健壮性)；当前版本已修复 |
| 链接时出现大量 `--enable-non-contiguous-regions discards section` 错误，最后 `ld terminated with signal 11` | IRAM 溢出。多半是打开了 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`：LVGL 9.5 会往 IRAM 放约 117 KB 混合函数，而 P4 可执行 SRAM 仅约 175 KB。保持该项关闭；若确需 IRAM 加速，先在 LVGL 配置中关闭用不到的 `CONFIG_LV_DRAW_SW_SUPPORT_*` 色彩格式 |

---

<p align="center"><sub>© 2026 OSPTEK 鱼鹰光电 · 本仓库资料采用 CC BY 4.0 许可</sub></p>
