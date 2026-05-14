# ESP32-P4 GPIO Allocation (WT0132P4-A1)

## Overview
This board is used as a **universal hub** with the following base modules:
- ESP32-S3 (SPI slave, networking)
- CC2652P1 (UART, Zigbee/Thread)

GPIOs are allocated considering:
- stable boot
- no conflicts with USB/debug
- scalability for future modules

---

# 1. Already Reserved GPIO

## ESP32-S3 (SPI slave)
| GPIO | Function |
|------|----------|
| IO18 | SPI SCLK |
| IO19 | SPI MOSI |
| IO20 | SPI MISO |
| IO21 | SPI CS |
| IO22 | IRQ |
| IO23 | RESET / EN |

## CC2652P1 (UART)
| GPIO | Function |
|------|----------|
| IO16 | UART TX |
| IO17 | UART RX |
| IO28 | RESET (optional) |
| IO29 | IRQ / WAKE (optional) |

---

# 2. Do NOT Use (Critical Pins)

| GPIO | Reason |
|------|--------|
| IO24 | USB FS |
| IO25 | USB FS + BOOT |
| IO34 | Strapping |
| IO36 | Strapping |
| IO37 | UART0 TX (debug) + strapping |
| IO38 | UART0 RX (debug) + strapping |
| IO51 | Onboard RGB LED |

---

# 3. Use With Caution

| GPIO | Notes |
|------|------|
| IO2–IO5 | JTAG (usable but disables debug) |
| IO7–IO8 | MIPI I2C (display/camera) |
| IO43 | Output-only (use for control signals) |

---

# 4. Recommended Expansion GPIO

## General-purpose (safe)
IO6, IO9, IO10, IO11, IO12, IO13, IO14, IO15  
IO26, IO27  
IO30, IO31, IO32, IO33  
IO39, IO40, IO41, IO42, IO44, IO45, IO46, IO47, IO48  
IO49, IO50, IO52, IO53, IO54  

---

# 5. Suggested Functional Groups

## SPI Expansion Bus (Radio / High-speed modules)
SCLK  -> IO30  
MOSI  -> IO31  
MISO  -> IO32  

CS0   -> IO33  
CS1   -> IO40  
IRQ0  -> IO39  
IRQ1  -> IO41  
IRQ2  -> IO42  

RST   -> IO44  
BOOT  -> IO45  

Use cases:
- ESP32-C6 (Thread / Matter)
- CC1101 (433 MHz)
- Other SPI devices

---

## UART Expansion
TX -> IO26  
RX -> IO27  

Use cases:
- GPS
- External MCU
- Debug port

---

## I2C Expansion
SDA -> IO9  
SCL -> IO10  
INT -> IO11  
RST -> IO12  

Use cases:
- Sensors
- RTC
- IO expanders

---

## LED / IR / Timing
WS2811 -> IO13  
IR_TX  -> IO14  
IR_RX  -> IO15  
CTRL   -> IO43 (output only)  

---

# 6. AUX Buttons / User Inputs

## Recommended GPIO for buttons
IO11  
IO12  
IO13  
IO14  
IO15  
IO26  
IO27  
IO30  
IO31  
IO32  

## Suggested mapping
RESET_BTN   -> IO35  (BOOT button on dev kit)
JOIN_BTN    -> IO12  
USER_BTN1   -> IO13  
USER_BTN2   -> IO14  

## Notes
- Use pull-up (internal or external)
- Active LOW (button to GND)
- Avoid strapping pins

---

# 7. Special Notes

- SD pins (IO39–IO48) can be reused as GPIO if SD is not used
- IO43 should be used as output/control only
- Do not mix high-speed SPI (ESP32-S3) with other devices
- Use a separate SPI bus for RF modules
- Keep some GPIO reserved for future reset/boot/IRQ needs

---

# 8. Design Philosophy

- Dedicated buses for critical modules (S3, CC2652P1)
- Separate SPI for RF
- Separate I2C for low-speed devices
- Group GPIOs by function
- Avoid strapping/debug pins where possible
