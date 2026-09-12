# Continue development on Umbrel

Updated 2026-09-12. Read `PROJECT_CONTEXT.md` for the design and controls.

## Current status

- Firmware compiled successfully on macOS with ESP-IDF 5.5.1 and the pinned dependencies. It has **never been flashed or hardware-tested**.
- The user has now connected the ESP32 directly to Umbrel. Discover its Linux serial device; do not reuse the macOS device path.
- Remote checkout: `/home/umbrel/dev/paper-audio`, SSH host `umbrel-codex`.
- Factory backup belongs in `.private/backups/factory-8mb.bin`, outside Git. It is 8,388,608 bytes; SHA-256: `b5e7f335daebb069812337b7b0b3b83b276fe75c4136b1650b3309470db84fc9`. Verify before flashing and keep private: flash images can contain credentials.

## Next work

1. Discover the attached ESP32 and serial permissions. Install/activate ESP-IDF 5.5.1 for Linux and rebuild; Mac build artifacts/toolchains are not portable.
2. Review the initial implementation before flashing. In particular: bound the vendor display BUSY waits; ensure idle/paused checkpoints do not supersede newer server progress; retain failed progress updates; ensure sleep expiry interrupts blocked buffering; review session lifecycle and MP3 decode failures; verify URL resolution preserves the `/audiobookshelf` prefix. These are review targets, not completed fixes. Misleading-indentation warnings were made nonfatal for the first build and should be cleaned up.
3. Resolve Audiobookshelf LAN access using Umbrel's existing app configuration. The backend responds internally, but the user-provided port 13378 was unavailable. USB attachment does not give the ESP32 access to Docker networking: it still needs a Wi-Fi-reachable endpoint. Preserve authentication and avoid public exposure.
4. Verify authenticated library/session/progress responses and MP3 HTTP range requests against installed Audiobookshelf 2.36.0. Keep credentials out of commits and logs.
5. Verify the backup, then bring up speaker at low volume, buttons, display, and battery power. Review the changed NVS partition layout before first flash.
6. Validate VBR seeking, chapters and tracks, timer, button holds, restart/resume, and network loss. Complete a 30-minute playback test while navigating the UI and verify server progress.

The older next-step list in `PROJECT_CONTEXT.md` predates the successful Mac build. A successful build is not evidence that playback works. Headphones, offline downloads, and speed adjustment remain deferred.
