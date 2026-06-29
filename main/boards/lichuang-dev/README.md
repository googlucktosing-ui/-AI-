# Lichuang ESP32-S3 Dev Board

This board profile targets the Lichuang practical ESP32-S3 development board from the local reference package:

- ES8311 speaker codec on I2C GPIO1/GPIO2 and I2S MCLK GPIO38, BCLK GPIO14, WS GPIO13, DOUT GPIO45
- ES7210 microphone ADC on the same I2C bus and I2S DIN GPIO12
- PCA9557 at address 0x19 for LCD CS, PA enable, and camera power-down
- ST7789 320x240 LCD on SPI3, FT5x06 touch, and DVP camera pins matching the upstream `lichuang-dev` profile

For first bring-up this profile uses a conservative 16 kHz input/output path and disables device-side AEC reference input. This matches the simplest voice path for wake word, ASR, TTS, and playback validation before tuning echo cancellation.

Recommended VM build:

```bash
cd ~/esp32/esp32-board/xiaozhi-esp32
. ~/esp-idf/export.sh
python scripts/release.py lichuang-dev
```

Manual build alternative:

```bash
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.lichuang-dev" build
```

Expected smoke-test order after flashing:

1. Boot and enter Wi-Fi provisioning with BOOT if needed.
2. Confirm LCD and touch initialize without blocking boot.
3. Confirm speaker output from TTS.
4. Confirm the "ni hao xiao zhi" wake word.
5. Confirm a normal spoken question reaches ASR and receives a TTS reply.
