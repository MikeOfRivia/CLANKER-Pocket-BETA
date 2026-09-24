# CLANKER Pocket BETA

Clean-room firmware for the Waveshare ESP32-S3 e-Paper 3.97" board.

## Build philosophy

This repo intentionally starts small. Hardware primitives are proven one at a time before higher-level UI or application behavior is added.

Initial bring-up order:

1. Boot and e-paper display
2. Physical buttons
3. Microphone capture
4. Wi-Fi / HTTPS
5. OpenAI transcription
6. CLANKER interaction layer
7. E-reader features

The previous FolloUp-derived repository is reference material only; this repository is the product rebuild.
