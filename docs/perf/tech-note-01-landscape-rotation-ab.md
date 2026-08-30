# 横屏（90° PPA 旋转）性能 A/B——AM319M262928ZS @ ESP32-P4

> 状态：已完成双轮实测。方法论借鉴 `piconetmon/projects/11-esp32p4-time-center` 的
> L0 系列技术笔记：单变量 A/B、冻结基线、双轮可重复性、结论先行、门禁如实记录。

## 1. 结论先行

1. 在 esp_lvgl_adapter 0.6.4 的 `TRIPLE_PARTIAL` 模式下开 `ESP_LV_ADAPTER_ROTATE_90`，
   横屏（逻辑 928×262）的 `lv_demo_benchmark` 全场景平均 **55 FPS / 帧均 12 ms**，
   与竖屏基线（56 FPS / 12 ms）几乎持平——**PPA 硬件旋转把横屏的转置代价压到了
   均值不可见的水平**，双轮 0 次看门狗、0 个绘制错误。
2. 旋转路径是 adapter 的 `ppa_do_scale_rotate_mirror()`：每个脏区条带从 SRAM 渲染
   缓冲经 PPA SRM 引擎旋转后直接落位写入 PSRAM 帧缓冲（无中间缓冲、无 CPU 转置）。
   `lvgl_bridge_v9.c` 中 UI 的 90° 映射为 PPA 的 270°，RGB565 直通。
3. 逐场景有再分布：文字满屏场景变慢（Screen sized text 29→23 FPS，条带几何变为
   928 宽 × ~36 行，文字管线对宽条带不利）；Widgets demo 反而变快（30→33 FPS，
   横屏布局改变了内容分布）；滚动场景 60→52。**橫屏与竖屏的 benchmark 内容布局
   本身不同，这是端到端朝向对比，不是纯旋转开销的隔离测量**——纯旋转开销的
   上界可从"多数场景 flush 不升反降"推断为 ≈0–3 ms/帧。
4. 官方背书：Espressif 在 esp-iot-solution#628 明确表示 adapter 是 port 的功能超集、
   "recommend migrating to it"；本实验同时验证了该迁移路线在本模组上的可行性。

## 2. 冻结条件

除 `ADAPTER_ROTATION` 一个宏外，两组配置完全一致（同一 sdkconfig、同一源码树）：

| 项目 | 值 |
|---|---|
| 硬件 | ESP32-P4 rev v1.3（360 MHz）+ AM319M262928ZS（CO6300，262×928 RGB565）|
| 面板时序 | DPI 20 MHz，60.01 Hz，2-lane MIPI-DSI |
| 组件 | esp_lvgl_adapter 0.6.4 + LVGL 9.5.0（registry 原版）+ IDF 6.1 |
| 渲染 | `TRIPLE_PARTIAL`（3 幅 PSRAM FB + SRAM 局部条带），`buffer_height=128`（67 KB/条带，两组同大小） |
| 线程 | `LV_OS_FREERTOS` + `LV_DRAW_SW_DRAW_UNIT_CNT=2`，UI 任务钉 core 1 |
| 内存 | `LV_USE_CLIB_MALLOC=y`（64 KB 内置池 OOM 活锁的根因修复，见示例 README 2026-08-30 记录） |
| 负载 | `lv_demo_benchmark`（LVGL 9.5），冷启动自动运行 |
| 基线 | ROTATE_0，日志 `bench-adapter-clib.log`（2026-08-30） |
| 实验组 | ROTATE_90，app bin SHA-256 前缀 `8c88bb368046ffc1`，双轮冷启动 |

FB 数量两组同为 3（`display_manager_required_frame_buffer_count`：TRIPLE_PARTIAL 与
非零旋转都要求 3）；LVGL 逻辑分辨率由 adapter 依旋转自动交换（928×262），绘制
缓冲按物理宽 × `buffer_height` 像素数分配，两组字节数相同——A/B 变量唯一。

## 3. 数据路径（ROTATE_90）

```text
LVGL task (core 1) + 2 个 SW draw 线程
  按逻辑朝向 928×262 渲染脏区 → SRAM 条带（33,536 px/条）
  → flush → lvgl_bridge_v9: rotate_copy_strided_region()
  → ppa_do_scale_rotate_mirror(BLOCKING)  // PPA SRM，UI 90° = PPA 270°
       in  = SRAM 条带（RGB565）
       out = 当前 PSRAM 帧缓冲的旋转后目标窗口
  → 三缓冲轮换 → DSI video 扫描
```

对照 time-center L0.2 的教训：esp_lvgl_port 的旋转路径是"阻塞 PPA → 中间缓冲 →
panel submit/copy"两段串行（占其全屏 render 的 81%）；adapter 少了中间缓冲与
submit 拷贝，这是本实验旋转近零开销的结构性原因。进一步的收益空间（PPA 改
NON_BLOCKING 与下一条带渲染重叠，time-center L0.4 实测 +71~86%）在 LVGL 9.5 单
`disp->flushing` 语义下上界为 in-flight=1，属将来选项，本轮不动。

## 4. 双轮实测

采集方式：CH343 串口 115200，脚本复位后整轮采集；LVGL 汇总表由
`LV_USE_LOG` + `LV_USE_PERF_MONITOR_LOG_MODE` 打到串口。原始日志在会话
scratchpad（`bench-adapter-rot90-run{1,2}.log`），不入库。

### 4.1 ROTATE_90 双轮 vs ROTATE_0 基线（Avg. FPS / 帧均 ms / render ms / flush ms）

| 场景 | ROTATE_0 基线 | ROT_90 Run 1 | ROT_90 Run 2 |
|---|---|---|---|
| Empty screen | 59 / 12 / 1 / 11 | 59 / 14 / 2 / 12 | 59 / 13 / 1 / 12 |
| Moving wallpaper | 60 / 14 / 3 / 11 | 61 / 10 / 3 / 7 | 61 / 10 / 3 / 7 |
| Single rectangle | 60 / 15 / 0 / 15 | 59 / 13 / 0 / 13 | 60 / 12 / 0 / 12 |
| Multiple rectangles | 60 / 14 / 1 / 13 | 60 / 13 / 1 / 12 | 60 / 14 / 1 / 13 |
| Multiple RGB images | 60 / 14 / 0 / 14 | 60 / 14 / 1 / 13 | 60 / 14 / 1 / 13 |
| Multiple ARGB images | 60 / 14 / 3 / 11 | 60 / 14 / 3 / 11 | 60 / 14 / 3 / 11 |
| Rotated ARGB images | 60 / 9 / 5 / 4 | 60 / 8 / 5 / 3 | 61 / 9 / 5 / 4 |
| Multiple labels | 60 / 13 / 3 / 10 | 59 / 13 / 4 / 9 | 60 / 12 / 4 / 8 |
| Screen sized text | 29 / 31 / 30 / 1 | 23 / 40 / 35 / 5 | 23 / 40 / 35 / 5 |
| Multiple arcs | 60 / 10 / 0 / 10 | 60 / 9 / 0 / 9 | 60 / 11 / 0 / 11 |
| Containers | 61 / 3 / 2 / 1 | 61 / 3 / 2 / 1 | 60 / 3 / 2 / 1 |
| Containers with overlay | 56 / 15 / 14 / 1 | 55 / 16 / 11 / 5 | 55 / 15 / 11 / 4 |
| Containers with opa | 61 / 4 / 3 / 1 | 60 / 4 / 3 / 1 | 61 / 4 / 3 / 1 |
| Containers with opa_layer | 61 / 6 / 6 / 0 | 61 / 6 / 5 / 1 | 61 / 6 / 5 / 1 |
| Containers with scrolling | 60 / 13 / 11 / 2 | 52 / 16 / 11 / 5 | 52 / 16 / 11 / 5 |
| Widgets demo | 30 / 22 / 21 / 1 | 33 / 22 / 19 / 3 | 33 / 22 / 19 / 3 |
| **全场景平均** | **56 / 12 / 6 / 6** | **55 / 12 / 6 / 6** | **55 / 12 / 6 / 6** |

可重复性：两轮全场景平均逐项相同；逐场景 Avg. FPS 最大漂移 1 FPS（≤2%），满足
借鉴的 ≤3% 双轮漂移门禁。错误门禁：双轮 `task_wdt` / `draw task failed` / `Guru` /
`abort` 均为 0。

### 4.2 解读

- **均值层面旋转免费**：多数场景 flush（含 PPA 旋转）不高于甚至低于竖屏的
  DMA 拷贝路径。PPA 128 B burst 的旋转写与顺序拷贝在此带宽区间同量级。
- **文字满屏是横屏的最劣场景**（23 FPS）：竖屏该场景本就是 TRIPLE_PARTIAL 的
  弱项（29 FPS vs 直写 47 FPS），横屏条带变宽变矮进一步放大。文档/长文型 UI
  若以横屏为主，需在真实界面上复测（见 §6）。
- **Widgets 类真实负载横屏更快**（33 vs 30 FPS），与"横屏拖拽的脏区高度更小"
  的几何直觉一致。

## 5. 已吸收的 time-center 实践

| 实践 | 本文应用 |
|---|---|
| 单变量 A/B + 冻结基线 | 仅 `ADAPTER_ROTATION` 一个宏，配置零 diff |
| 双轮可重复性 | ROTATE_90 跑两轮冷启动对照 |
| 结论先行 + 门禁如实记录 | §1；错误计数为零才计入结果 |
| motion FPS 意识 | 沿用其结论：sysmon 静态 60 FPS ≠ 面板呈现 60 个新帧；本文只用 benchmark 逐场景 Avg. FPS |
| 待复用（未实施） | `--wrap` 归因仪表（draw/PPA/submit 分解）、异步 PPA 双槽所有权流水线（LVGL 9.5 in-flight 上界 1）、USB 串口中断驱动防丢字节 |
| 反向教训 | 其 AXS15260 模组不支持 RGB565 扫描（L0.5 被否）；本模组 RGB565 全链路已验证——不要轻易切 RGB888，带宽 +50% |

## 6. 遗留与下一步

- [ ] **人工画面/触摸门禁**：横屏画面方向、无花屏/撕裂需人眼确认；触摸坐标经
      adapter 输入变换后的正确性未测（benchmark 不用触摸）。
- [ ] 真实 UI（时钟/表盘类）在横屏下的 motion FPS 实测，替代 benchmark 代理负载。
- [ ] 若旋转成为瓶颈再评估：PPA NON_BLOCKING + 条带渲染重叠（time-center L0.4
      蓝图），或等 LVGL ≥9.6 的调度语义变化。
- [ ] `esp_lvgl_port → esp_lvgl_adapter` 的正式迁移决策（官方推荐迁移，
      esp-iot-solution#628；本仓库示例暂留 port，adapter 试验工程另存）。

## 参考

- 本仓库示例 README「渲染模式」章（2026-08-30 活锁根因与内存矩阵记录）
- [esp-iot-solution#628](https://github.com/espressif/esp-iot-solution/issues/628) 官方定位表态
- [lvgl/lvgl#10369](https://github.com/lvgl/lvgl/pull/10369) OOM 活锁上游修复（9.6）
- `piconetmon/projects/11-esp32p4-time-center/docs/` L0.2/L0.4/L0.5 技术笔记
