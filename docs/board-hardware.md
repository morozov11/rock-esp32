# JC4880P443C_I_W Hardware Reference

This reference documents the hardware architecture, peripherals, and pin assignments for the **JC4880P443C_I_W** board based on vendor schematics and specifications from `D:\work\JC4880P443C_I_W`.

## 1. System Overview

- **Host SoC**: Espressif ESP32-P4 (RISC-V dual-core @ 400 MHz, hardware revision v1.3)
- **Wi-Fi / Bluetooth Companion**: Espressif ESP32-C6 (connected over SDIO 4-bit)
- **Flash**: 16 MB SPI Flash (Boya BY25Q128AS / generic driver)
- **PSRAM**: 32 MB High-speed PSRAM
- **Crystal**: 40 MHz
- **Power**: 5 V via USB Type-C (recommended $\ge 600\text{ mA}$)

---

## 2. Pin Mapping Summary

### Display (ST7701 4.3" MIPI-DSI)
- **Interface**: MIPI-DSI (2 data lanes @ 750 Mbps)
- **PHY Power**: Internal LDO Channel 3 @ 2.5 V (`esp_ldo_acquire_channel`)
- **Resolution**: 480x800 native portrait (software-rotated by LVGL to 800x480 landscape)
- **Backlight PWM**: `GPIO 23` (LEDC, 5 kHz, ~70% default duty)
- **Reset (RST)**: `GPIO 5`

### Capacitive Touch (Goodix GT911)
- **Interface**: I2C Master (port 1)
- **SDA**: `GPIO 7`
- **SCL**: `GPIO 8`
- **Interrupt (INT)**: NC / Internal Pullup
- **Reset (RST)**: NC / Internal Pullup

### Wi-Fi / Bluetooth Companion (ESP32-C6 over SDIO)
- **Protocol**: ESP-Hosted (version 3.0.7)
- **SDIO CLK**: `GPIO 18`
- **SDIO CMD**: `GPIO 19`
- **SDIO D0**: `GPIO 14`
- **SDIO D1**: `GPIO 15`
- **SDIO D2**: `GPIO 16`
- **SDIO D3**: `GPIO 17`
- **C6 Hardware Reset**: `GPIO 54` (Active LOW)

### Audio Codec (ES8311 & Power Amplifier)
- **Control Interface**: I2C (shares `GPIO 7` SDA and `GPIO 8` SCL, I2C address `0x18`)
- **I2S SCLK (BCLK)**: `GPIO 12`
- **I2S MCLK**: `GPIO 13`
- **I2S LCLK (WS)**: `GPIO 10`
- **I2S DOUT (Speaker DAC)**: `GPIO 9`
- **I2S DSIN (Microphone ADC)**: `GPIO 48`
- **Power Amplifier Enable (PA)**: `GPIO 11`

### MicroSD Card (SDMMC 4-Bit)
- **Slot**: TF Card slot (FAT32, $\le 32\text{ GB}$)
- **SD D0**: `GPIO 39`
- **SD D1**: `GPIO 40`
- **SD D2**: `GPIO 41`
- **SD D3**: `GPIO 42`
- **SD CLK**: `GPIO 43`
- **SD CMD**: `GPIO 44`

### RS-485 Serial Port
- **TXD**: `GPIO 26`
- **RXD**: `GPIO 27`
- **Mode**: Half-duplex with auto direction control

### Camera Interface (MIPI-CSI)
- **Sensor**: OV02C10 (2-lane MIPI-CSI)
- **Supported in vendor baseline**: ESP-IDF 5.5.4 via `esp_cam_sensor` v2.1.0

### USB Connectors
- **USB2**: Native USB Serial/JTAG directly connected to ESP32-P4 (used for programming, flashing, and IDF monitor).
- **USB1**: USB OTG / Host interface.

---

## 3. Recovery and Vendor Artifacts

All vendor recovery files and tools are archived in:
`D:\work\JC4880P443C_I_W\8-Burn operation\Burn files`

- **JC-C6-slave_v2.3.2.bin**:
  `SHA-256: F9EC1205C5FA1E497032DEDCEE33D2AB9560BD2768FC1EA06ACB6D8E2773F80C`
- **C6-JC4880P443-2.1.10.bin**:
  `SHA-256: 7B68C4CFF6E3CF288896A89B4A09379396F62C6BFBFBDC3984B9E142CFB4DB0C`
- **P4-JC4880P443C_I_W_V2.2.bin**: Complete vendor P4 recovery binary.
