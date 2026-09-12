# Third-party sources

The e-paper driver is taken from Waveshare’s V2 08_Audio_Test, commit 9957d0f4fc7cd40d1d42880cb1b74a8d6782a6c2.
Source: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
Local changes: bounded BUSY waits, framebuffer initialization, safe display refresh, and cleanup.
The upstream repository does not declare a top-level redistribution license; obtain permission before redistributing these vendor files as a product.

font8x8_basic.h: public-domain font by Daniel Hepper / IBM VGA heritage.
https://github.com/dhepper/font8x8

Espressif components retain their licenses in managed_components and dependencies.lock.
