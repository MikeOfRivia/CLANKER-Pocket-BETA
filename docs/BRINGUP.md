# Bring-up gates

## Phase 1 - display + direct buttons
Current milestone.

Expected boot screen:
- CLANKER
- POCKET BETA
- DISPLAY OK
- LAST BUTTON: NONE
- PRESS COUNT: 0

Each physical BOOT / Up / Select / Down press updates the displayed button name and increments the press count.

No input dispatcher. No recording state machine. No Wi-Fi. No overlays. No application navigation.

## Phase 2 - microphone capture
Only after Phase 1 passes all four direct GPIO buttons repeatedly.

BOOT becomes push-to-talk. Hold captures raw PCM. Release displays duration, sample count, peak and RMS level. No network.

## Phase 3 - network
Prove Wi-Fi and a small HTTPS request independently of audio.

## Phase 4 - OpenAI transcription
Send the already-proven PCM clip and display request phase, HTTP status and raw error/result.

## Phase 5 - CLANKER
Add response backend and interaction UI.

## Phase 6 - reader
Add e-reader functionality only after the appliance foundation is stable.
