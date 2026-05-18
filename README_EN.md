# 3.19" 272×928 AMOLED MIPI module (CO6300) — documentation & samples

**简体中文：** [`README.md`](README.md)

---

> This repository provides **sample projects** for this module, together with datasheets, specifications, and interface / bring-up documentation for selection reference and integration.

## Product overview

| Item | Description |
|:--|:--|
| Module | 3.19-inch **AMOLED** panel, **272×928** resolution |
| Interface | **MIPI** |
| Driver IC | **CO6300** |
| Spec ID | **`3.19-amoled-272x928-mipi-co6300`** is the common product designation in documentation |

---

## Repository layout

### Top-level

| Path | Contents |
|:--|:--|
| `docs/` | Datasheets, specifications, interface and initialization documentation |
| `examples/` | **Sample projects** |

### `examples/` layout

| Location | Description (internal package folder) |
|:--|:--|
| `examples/` root | **IDF代码** (esp-lvgl-port + LVGL9) |

### Sample project paths

| Description | Path |
|:--|:--|
| esp-lvgl-port + LVGL9 | `examples/P4-IDF_CO6300-MIPI_ESP-LVGL-PORT_V9/` |
