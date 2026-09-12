# Paper Audio: project context and handoff

Updated 2026-09-12. This file is intentionally committed so work can continue from an Umbrel-hosted Codex session.

## Goal

Turn the Waveshare ESP32-S3-ePaper-1.54 V2 into a battery-powered audiobook player. Audiobookshelf remains the source of truth for books and listening progress. The first release uses the onboard speaker; headphone output is later work.

## Hardware facts

- Connected board: Waveshare ESP32-S3-ePaper-1.54 V2.
- The connected board identifies as ESP32-S3-PICO-1, with 8 MB flash and 8 MB PSRAM.
- Serial device observed on macOS: `/dev/cu.usbmodem2101`.
- V2 audio pins from the vendor example: I2C SDA 47, SCL 48; I2S MCLK 14, BCLK 15, WS 38, DOUT 45, DIN 16; amplifier enable GPIO 46.
- E-paper: SPI CS 11, DC 10, SCLK 12, MOSI 13, reset 9, busy 8; power GPIO 6.
- Audio power GPIO 42; battery power latch GPIO 17; BOOT GPIO 0; PWR GPIO 18.
- The board accepts a 3.7 V LiPo through its dedicated battery connector and includes charging/power management.
- ESP32-S3 Bluetooth is BLE only; ordinary Bluetooth headphone A2DP is not available directly.
- The existing USB-C connector is documented as programming/logging and is not assumed to be a headphone host port.

## Umbrel / Audiobookshelf

- Umbrel host reached through SSH alias `umbrel-codex`; its Tailscale address is `100.74.11.40`.
- Audiobookshelf version: 2.36.0.
- Container address on Umbrel's Docker network: `10.21.0.6`.
- Audiobookshelf is healthy inside Umbrel at `http://10.21.0.6/status`.
- The user normally opens `http://umbrel.local:13378/audiobookshelf/login`.
- At the time of implementation, port 13378 was not reachable from the Mac/LAN even though the app was healthy internally. The Umbrel generated compose file publishes no host port for the Audiobookshelf web container; investigate Umbrel's app proxy/routing rather than exposing the container directly.
- Audiobook storage is `/home/umbrel/umbrel/home/Downloads/audiobooks` and currently contains MP3 files (plus an EPUB and JPG).

## Intended user experience

Phone setup page served by the ESP32:

1. Enter 2.4 GHz Wi-Fi credentials, Audiobookshelf base URL, and either an API token or account login.
2. Browse paginated audiobook libraries.
3. Select one book; the device loads it and starts paused or playing according to the current implementation.

On-device controls:

- Playback: BOOT tap toggles play/pause; PWR tap opens the menu.
- Menu: PWR advances, BOOT selects, BOOT hold returns.
- Menu actions: rewind 30 seconds, forward 30 seconds, volume, chapters/tracks, sleep timer, return.
- PWR hold saves progress and powers down.

## Implementation plan

- Keep network buffering, MP3 decoding, codec output, button handling, and e-paper rendering in separate tasks.
- Decode MP3 with Espressif `esp_audio_codec` and drive the ES8311 through `esp_codec_dev`; stereo input is mixed to mono for the onboard speaker.
- Use HTTP range requests and a sparse MP3 frame index for accurate seeking, with one second of preroll for the MP3 bit reservoir.
- Create/close Audiobookshelf playback sessions and sync progress every 15 seconds, on pause/seek/book change, and before shutdown.
- Keep a durable local checkpoint and reconcile it against the server's last update timestamp.
- Store credentials in NVS only. Never log or commit them.
- Start paused after reboot when restoring the last book; pause and reconnect after Wi-Fi loss.

## Current repository state

- `main/board.cpp`: V2 board pin/power setup, ES8311 speaker output, battery ADC, buttons, e-paper rendering.
- `main/core.*`: MP3 header/index helpers, button/menu state model, URL validation, checkpoint reconciliation.
- `main/network.cpp`: Wi-Fi/AP setup, setup/status/library/book web API, mobile setup page integration.
- `main/player.cpp`: HTTP range stream, MP3 decoder, speaker pipeline, Audiobookshelf sessions/progress, seek and resume logic.
- `main/web/index.html`: phone setup and library selection UI.
- Vendor e-paper driver files are copied from Waveshare's V2 `08_Audio_Test` example; see `THIRD_PARTY.md`.

## Next steps on Umbrel

1. Install/activate ESP-IDF 5.5.1 and CMake/Ninja on the development host.
2. Build and fix compile/API mismatches against the pinned component versions.
3. Check the exact Audiobookshelf API response shapes for version 2.36.0, especially login, playback session, `audioTracks`, progress, and session sync.
4. Fix Umbrel LAN routing so the ESP32 can reach the configured Audiobookshelf base URL. Prefer the app proxy path or a controlled reverse proxy; do not publish the Docker container directly without understanding Umbrel's firewall/auth behavior.
5. Flash only after confirming the factory backup at `.private/backups/factory-8mb.bin`; verify low-volume speaker tone, display, buttons, Wi-Fi setup, then MP3 playback.
6. Test one short MP3, variable-bitrate seeking, multiple tracks, progress sync, reboot resume, Wi-Fi loss, and battery power-off.

## Known limitations

- No verified hardware test has been run yet.
- The current client is designed around MP3; add transcoding or other codec support later if the library requires it.
- No offline cache/download support.
- No USB-C or Bluetooth headphone output.
- The factory flash backup is local and excluded from Git; its SHA-256 is recorded in the development notes, not the public repository.
