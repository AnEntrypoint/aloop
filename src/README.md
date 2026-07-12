# src/ — the aloop runtime

The native binary: the ported loop engine, the in-process LV2 host, Link
integration, WiFi/AP control, and MIDI. All of it runs as one process with
per-core RT threads (see `docs/ARCHITECTURE.md`).

| Dir | Role |
|-----|------|
| `dsp/` | The loop engine + effects ported from `../looper` (Circle-free, allocation-free). Driven by the RT audio callback. |
| `host/` | The in-process LV2 host — `dlopen`s LV2 bundles and calls `run()` inside the audio callback (no JACK graph; see ADR-002). |
| `link/` | Ableton Link integration — the official lib on the control core, handing tempo/phase to audio via the lock-free snapshot. |
| `control/` | Control plane: MIDI (ALSA rawmidi), the APC param mapping, telemetry. |
| `net/` | WiFi/AP orchestration hooks (autoAP switch coordination with hostapd/wpa_supplicant). |
