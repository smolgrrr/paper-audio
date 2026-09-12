# Paper Audio

Firmware for a Waveshare **ESP32-S3-ePaper-1.54 V2** audiobook player. The device streams MP3 audiobooks from Audiobookshelf over Wi-Fi and plays them through the board's onboard ES8311 speaker path.

The phone is used to configure the device and choose a book. Playback is controlled from the board buttons:

- BOOT tap: play/pause
- PWR tap: open/advance the playback menu
- Menu BOOT: select; menu PWR: next option
- BOOT hold: go back
- PWR hold: save progress and power off

USB/Bluetooth headphones, offline downloads, and playback speed are deliberately deferred.

## Status

This is an active work-in-progress. The repository contains the initial firmware architecture, board drivers, speaker output path, setup web UI, Audiobookshelf client, MP3 stream/decode pipeline, progress checkpointing, and button/menu model. Build and hardware validation are still required before flashing production firmware.

## Build

Use ESP-IDF 5.5.1 and target `esp32s3`:

```sh
. "$HOME/.espressif/esp-idf-v5.5.1/export.sh"
idf.py set-target esp32s3
idf.py build
```

See [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) for the implementation plan, hardware facts, Audiobookshelf deployment details, and the next integration steps.

## Safety

Do not commit Wi-Fi passwords, Audiobookshelf tokens, `sdkconfig`, build output, or the local factory flash backup. These are excluded by `.gitignore`.
