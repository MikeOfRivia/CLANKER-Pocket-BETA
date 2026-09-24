# Bring-up gates

## Phase 1 - display + direct buttons — PASSED

Physically proven on-device:
- SSD1677 e-paper initializes and refreshes
- BOOT / GPIO0
- radial Up / GPIO4
- radial Select / GPIO5
- radial Down / GPIO6
- repeated direct button presses update the display reliably

This is the first known-good CLANKER Pocket hardware baseline.

## Phase 2 - microphone capture — CURRENT

BOOT is now push-to-talk.

Expected behavior:
1. Hold BOOT.
2. Screen changes to RECORDING.
3. Speak.
4. Release BOOT.
5. Screen reports:
   - sample count
   - captured PCM duration
   - peak amplitude
   - RMS amplitude
   - whether the 10-second buffer limit was reached

The raw PCM remains in PSRAM after release until the next recording. There is still no Wi-Fi and no transcription in this phase.

Success criteria:
- Multiple recordings work consecutively without reboot.
- Duration roughly tracks hold time.
- Sample count is non-zero.
- Peak and RMS are non-zero when speaking.
- Radial Up / Select / Down still work after recording.

## Phase 3 - network

Prove Wi-Fi and a small HTTPS request independently of audio.

## Phase 4 - OpenAI transcription

Send the already-proven PCM clip and display request phase, HTTP status and raw error/result.

## Phase 5 - CLANKER

Add response backend and interaction UI.

## Phase 6 - reader

Add e-reader functionality only after the appliance foundation is stable.
