# docs/ 的 Markdown 版本

本目录是 [`../`](../) 下各 PDF 的 Markdown 转换结果，方便全文检索、在 IDE / AI 工具中直接引用。**以 PDF 原件为准**，本目录仅作查阅辅助。

| 目录 | 来源 PDF | 页数 |
| ---- | -------- | ---- |
| [`AM_319_M262928_ZS_3a5effae7d/`](./AM_319_M262928_ZS_3a5effae7d/AM_319_M262928_ZS_3a5effae7d.md) | 产品规格书 `AM_319_M262928_ZS_3a5effae7d.pdf` | 1 |
| [`CO_6300_Datasheet_V0_00_20240827_for_customer_d6da76ca4d/`](./CO_6300_Datasheet_V0_00_20240827_for_customer_d6da76ca4d/CO_6300_Datasheet_V0_00_20240827_for_customer_d6da76ca4d.md) | 驱动 IC 数据手册 `CO_6300_Datasheet_V0_00_20240827_for_customer_d6da76ca4d.pdf` | 243 |
| [`CST_3530_V1_0_adb72690f0/`](./CST_3530_V1_0_adb72690f0/CST_3530_V1_0_adb72690f0.md) | 触摸 IC 数据手册 `CST_3530_V1_0_adb72690f0.pdf` | 12 |
| [`3.19寸262x928 AMOLED转接板/`](./3.19%E5%AF%B8262x928%20AMOLED%E8%BD%AC%E6%8E%A5%E6%9D%BF/3.19%E5%AF%B8262x928%20AMOLED%E8%BD%AC%E6%8E%A5%E6%9D%BF.md) | 转接板原理图 `3.19寸262x928 AMOLED转接板.pdf` | 2 |

每个目录下：`<同名>.md` + `images/`（按内容哈希命名的 JPG，md 内相对引用）。

## 转换方式

- 工具：[MinerU](https://github.com/opendatalab/MinerU) 3.4.5，`vlm-http-client` 后端（MinerU2.5 VLM），2026-08-28 转换。
- 调用：`POST /file_parse`，参数 `backend=vlm-http-client`、`return_md=true`、`return_images=true`、`response_format_zip=true`；解包后去掉中间层 `vlm/`。
- 表格以 HTML `<table>` 形式保留；公式为 LaTeX（`$…$`）。
- 图片下方的 `<details><summary>text_image</summary>` 块是 VLM 对图内文字的识读，对原理图 / 机械图纸这类图形内容**识别噪声较多**，仅供检索，不要据此判断尺寸或网络名——请看图片或 PDF 原件。
