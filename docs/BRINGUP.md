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

## Phase 2 - microphone capture — PASSED

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

## Phase 3 - network — PASSED

If no saved Wi-Fi credentials exist, the device starts an open provisioning AP named `CLANKERBETA-XXXXXX` and shows `192.168.4.1` on-screen. Enter SSID/password there; the device saves them to NVS and reboots.

With Wi-Fi connected, the firmware performs an HTTPS GET to `https://example.com/` using the ESP-IDF certificate bundle and shows the returned HTTP status on the e-paper display. Radial SELECT reruns the HTTPS probe.

Success criteria:
- Provisioning survives reboot.
- Wi-Fi reconnects from NVS without re-entry.
- HTTPS returns a real HTTP status (normally 200).
- BOOT microphone capture still works.
- Up / Select / Down remain responsive after both recording and HTTPS activity.

## Phase 4 - OpenAI transcription — PASSED

Provisioning now collects Wi-Fi credentials and an OpenAI API key together.

After BOOT capture completes:
1. The previously proven PCM remains in PSRAM.
2. The device renders `TRANSCRIBING`.
3. It synchronously sends a WAV multipart request to OpenAI using `gpt-4o-mini-transcribe`.
4. It renders the actual HTTP status and either transcript text or the provider/transport error.

This phase is deliberately synchronous. While the HTTP request is active, button input is blocked. That is acceptable for bring-up because it removes task/callback/state-machine coupling while we prove the complete audio -> TLS -> OpenAI path.

Success criteria:
- Provisioning saves both Wi-Fi and API key.
- A spoken clip produces a non-empty transcript.
- HTTP status is visible.
- Failure details are visible instead of hidden.
- Multiple capture/transcribe cycles work consecutively.
- Up / Select / Down remain usable after a completed request.

## Phase 5 - CLANKER — CURRENT

After a successful transcription, the transcript is sent for a concise CLANKER text response.

The device renders:
- CLANKER THINKING while the request is active
- the recognized user text
- the CLANKER response
- HTTP/error details if the response request fails

The response is intentionally short for the 480x800 e-paper display.

Success criteria:
- Spoken request transcribes correctly.
- CLANKER returns a non-empty response.
- The response renders legibly.
- Multiple ask cycles work consecutively.
- Recording, networking, transcription, and buttons still work afterward.

## Phase 6 - reader

Add e-reader functionality only after the appliance foundation is stable.
