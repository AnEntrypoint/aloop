# aloop <-> esp-idf-link mesh: first-contact test plan

**Status: not yet run.** This is prep for the first-ever test of aloop
(Pi 4, hardware looper) and esp-idf-link (ESP32) Link peers on the same
network. Nothing below has been executed against real hardware yet — treat
every "expect" as a hypothesis to falsify, not a known result.

## Why this doc exists

aloop and esp-idf-link were built independently. Both vendor real Ableton
Link and speak the identical wire protocol (verified: `Messages.hpp` is
byte-identical between the two trees, and both use Link's own hardcoded
multicast group `224.76.78.75:20808` — this cannot drift, it's baked into
Link itself). Both implement standards-compliant tempo-write
(`LinkBridge::proposeTempo` on aloop's side, ordinary `setTempo`/
`commitAppSessionState` on esp-idf-link's side) — so **two-way sync should
work automatically once discovery completes**, no protocol-level glue code
needed between the two projects specifically.

The real open question is **discovery**, not protocol compatibility:
whichever device is a WiFi access point, does Link's UDP multicast traffic
actually reach a station connected to it?

## What we already know, and what's still a guess

| Claim | Status |
|---|---|
| Both projects vendor real Ableton Link, same wire protocol | **Confirmed** (file diff) |
| Same multicast group/port | **Confirmed** (Link's own hardcoded constant) |
| aloop's Pi 4 WiFi chip is Broadcom BCM4343-series (`brcmfmac` driver) | **Confirmed** (docs/MIGRATION-MAP.md cites `CBcm4343Device` as the bare-metal reference this was ported from) |
| ESP32 SoftAP does not forward multicast between host and stations at all | **Confirmed for ESP32** (esp-idf-link's own `wifi_config.cpp` comments + its unicast-relay workaround exist because of this) |
| Broadcom `brcmfmac` AP-mode on Linux has the SAME structural gap (not just a client-isolation setting `ap_isolate=0` already addresses) | **UNKNOWN — this is what the live test below determines** |
| aloop's `autoap.sh` (STA-fail-count -> become AP) avoids a two-devices-both-AP race the way esp-idf-link's MAC-rank election does | **UNKNOWN — no peer-election logic found in autoap.sh; may deadlock if multiple aloop units boot with no existing AP** |

## Test 1: does standard Link discovery complete at all, AP role = aloop

1. Power on one aloop unit with no existing WiFi network available, so
   `autoap.sh` falls through STA failures and hosts its own AP (`aloop`
   SSID, `192.168.4.1/24`, per `src/net/config/hostapd.conf`).
2. Power on one esp-idf-link ESP32, configured to join that `aloop` SSID as
   a station (NOT scanning for its own `"ticker"` SSID — you may need to
   temporarily point esp-idf-link's STA join target at aloop's AP SSID for
   this test, since its own auto-election logic assumes a different SSID
   name).
3. On the aloop side, check Link peer count. aloop's telemetry surface
   (`docs/REMOTE-CONTROL.md` / the udp/4445 telemetry socket, see
   `src/control/telemetry.cpp`) should expose Link sync state
   (`AudioThread::Telemetry::linkSynced`, `bpm`) — poll it and confirm
   `linkSynced` flips true and `bpm` matches whatever the ESP32 is
   broadcasting.
4. On the ESP32 side, esp-idf-link's own serial log (or its own Link peer
   count, if exposed — check `main/link_sync.cpp` for a log line printing
   `numPeers()`) should also show 1 peer.
5. **If both sides see each other: standard Link discovery works fine with
   aloop as AP.** No relay/forwarding fix needed for this direction — the
   `ap_isolate=0` config-level fix was sufficient.
6. **If either side never sees the other: this is the AP-mode multicast
   gap, confirmed real on Broadcom too.** Proceed to the prepared fix
   (see "If Test 1 fails" below).

## Test 2: same as Test 1, but AP role = esp-idf-link (the ESP32 hosts)

Swap roles: ESP32 hosts its own SoftAP, aloop's `autoap.sh` joins it as a
station. Same peer-count check both directions.

esp-idf-link's own SoftAP is ALREADY known to have the multicast-forwarding
gap (that's why its unicast-relay workaround exists) — but that relay only
activates for peers it explicitly resolves and unicasts to. Confirm whether
its existing relay mechanism, built for interop with the older bare-metal
"Pi looper," also picks up aloop's own Link traffic correctly, or whether
it needs to be told about this new peer type.

## Test 3: cold-boot start-order / AP-role stability with 2+2 devices

With all four devices (2x aloop, 2x esp-idf-link) powered off, power them
on in several different orders and combinations across repeated trials:
- All four simultaneously
- Both aloop units first, ESP32s 30s later
- Both ESP32 units first, aloop units 30s later
- One of each first, the other pair 30s later

For each trial, confirm: exactly one device becomes AP, all others become
stations, and every device eventually shows 3 peers (all devices "N")
in its own Link session. Record which device won AP role each time —
if it's non-deterministic or sometimes results in two APs / zero APs
(a split network), that is exactly the race esp-idf-link's MAC-rank
election was built to prevent, and aloop's `autoap.sh` does not currently
have an equivalent (see the "if Test 3 fails" section).

## If Test 1 or 2 fails (AP-mode multicast gap confirmed on aloop)

Port esp-idf-link's relay technique into aloop's networking layer:
- esp-idf-link's `wifi_link_multicast_forward()` + `link_multicast_relay_task()`
  (see `main/wifi_config.cpp`) unicast-copy Link's multicast discovery
  packets to every associated station (if AP) or to the gateway (if STA),
  preserving original source IP.
- On Linux/hostapd, the equivalent is almost certainly simpler than the
  ESP32 workaround: a raw multicast-forwarding daemon (`smcroute`,
  `igmpproxy`, or a small custom relay using `SO_REUSEADDR` + explicit
  `sendto()` fan-out to each DHCP lease IP from `dnsmasq`'s lease file) run
  alongside `hostapd` whenever aloop is in AP mode. This does NOT require
  touching aloop's own C++ (`link_bridge.cpp`) at all — it's a networking-
  layer fix (systemd service / OpenRC init script alongside `hostapd`), so
  Ableton's own Link library on aloop's side is untouched.
- Do NOT attempt this port speculatively before Test 1/2 actually fail —
  `ap_isolate=0` may already be sufficient on Broadcom's AP-mode stack even
  though ESP32's SoftAP needed more. Confirm the gap is real first.

## If Test 3 fails (AP-role race/deadlock with 2+ devices)

Port esp-idf-link's deterministic MAC-rank election + self-healing
supervisor loop (`main/wifi_config.cpp`'s `wifi_supervisor_task`) into
aloop's `src/net/autoap.sh`:
- Each aloop unit needs to know its own WiFi MAC address (`cat
  /sys/class/net/wlan0/address`) and compare against any other AP-mode
  aloop units it can see (scan for the `aloop` SSID's BSSID before
  deciding to host).
- Lowest-MAC-wins (or highest, pick one convention and match esp-idf-link's
  for consistency) with a randomized/staggered hold-off before hosting, so
  simultaneous cold boots don't all decide to host at the same instant.
- A background loop that keeps re-scanning for a higher-priority AP even
  after this unit has started hosting, and gracefully tears down its own
  AP + rejoins as STA if a better-ranked peer appears — mirroring
  esp-idf-link's "only rescans while it has zero AP clients" caution (to
  avoid dropping already-connected peers by flapping off-channel).

## Next step

Run Test 1, 2, and 3 against real hardware and report the actual peer-count
/ AP-role results back here (or update this doc's "what we already know"
table directly) before writing any relay/election code — per the standing
rule that a fix only gets built once its target failure is actually
witnessed, not assumed from a chip-family resemblance alone.
