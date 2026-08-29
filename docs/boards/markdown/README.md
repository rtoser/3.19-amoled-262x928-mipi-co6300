# docs/boards/ 的 Markdown 版本

本目录是 [`../`](../) 下各开发板 / 外设 PDF 的 Markdown 转换结果，方便全文检索、在 IDE / AI 工具中直接引用。**以 PDF 原件为准**，本目录仅作查阅辅助。

| 目录 | 来源 PDF | 页数 |
| ---- | -------- | ---- |
| [`ESP32-P4-Module_使用指南2026.03.26/`](./ESP32-P4-Module_%E4%BD%BF%E7%94%A8%E6%8C%87%E5%8D%972026.03.26/ESP32-P4-Module_%E4%BD%BF%E7%94%A8%E6%8C%87%E5%8D%972026.03.26.md) | ESP32-P4 模组使用指南 `ESP32-P4-Module_使用指南2026.03.26.pdf` | 6 |
| [`ESP32P4模组基础底板V1.3/`](./ESP32P4%E6%A8%A1%E7%BB%84%E5%9F%BA%E7%A1%80%E5%BA%95%E6%9D%BFV1.3/ESP32P4%E6%A8%A1%E7%BB%84%E5%9F%BA%E7%A1%80%E5%BA%95%E6%9D%BFV1.3.md) | ESP32-P4 模组基础底板原理图 `ESP32P4模组基础底板V1.3.pdf` | 1 |
| [`ESP32-P4-Camera/`](./ESP32-P4-Camera/ESP32-P4-Camera.md) | ESP32-P4 摄像头板 `ESP32-P4-Camera.pdf` | 7 |
| [`SC2336_数据手册_V0.7/`](./SC2336_%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C_V0.7/SC2336_%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C_V0.7.md) | SmartSens SC2336 图像传感器数据手册 `SC2336_数据手册_V0.7.pdf` | 34 |
| [`C5双频天线/`](./C5%E5%8F%8C%E9%A2%91%E5%A4%A9%E7%BA%BF/C5%E5%8F%8C%E9%A2%91%E5%A4%A9%E7%BA%BF.md) | ESP32-C5 双频天线规格 `C5双频天线.pdf` | 3 |

每个目录下：`<同名>.md` + `images/`（按内容哈希命名的 JPG，md 内相对引用；未被 md 引用的裁图已删除）。

## 转换方式

- 工具：[MinerU](https://github.com/opendatalab/MinerU) 3.4.5，`vlm-http-client` 后端（MinerU2.5 VLM），2026-08-29 转换。
- 调用：`POST /file_parse`，参数 `backend=vlm-http-client`、`return_md=true`、`return_images=true`、`response_format_zip=true`；解包后去掉中间层 `vlm/`。
- 表格以 HTML `<table>` 形式保留；公式为 LaTeX（`$…$`）。
- 图片下方的 `<details><summary>text_image</summary>` 块是 VLM 对图内文字的识读，对原理图 / 机械图纸这类图形内容**识别噪声较多**，仅供检索，不要据此判断网络名或尺寸——请看图片或 PDF 原件。
- 原文件 `C5双频天线 .pdf`（扩展名前有一个空格）已改名为 `C5双频天线.pdf`。
