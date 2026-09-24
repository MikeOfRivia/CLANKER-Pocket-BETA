# Hardware contract

Target: Waveshare ESP32-S3 e-Paper 3.97 inch, 800x480, SSD1677.

## Verified/preserved pin map

- BOOT / PTT: GPIO0
- Radial Up: GPIO4
- Radial Select: GPIO5
- Radial Down: GPIO6
- E-paper BUSY: GPIO3
- E-paper RESET: GPIO46
- E-paper DC: GPIO9
- E-paper CS: GPIO10
- E-paper MOSI: GPIO12
- E-paper SCK: GPIO11
- Shared sensor/PMIC I2C SDA: GPIO41
- Shared sensor/PMIC I2C SCL: GPIO42
- AXP2101 PMIC: 0x34
- PMIC IRQ: GPIO38
- ES8311 I2S MCLK: GPIO13
- ES8311 I2S BCLK: GPIO14
- ES8311 I2S WS: GPIO47
- ES8311 I2S DIN: GPIO21
- ES8311 I2S DOUT: GPIO48
- Speaker PA enable: GPIO39

## BETA rule

No higher-level feature is added until the lower hardware layer is visibly proven on-device.
