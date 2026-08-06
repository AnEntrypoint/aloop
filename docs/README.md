# docs/ — the design record

Read in this order:
1. `FEASIBILITY.md` — the witnessed study that justified the original
   Circle→Linux migration.
2. `ARCHITECTURE.md` — how aloop is structured and *why* (traces to the study;
   includes the newer instrument-layer DSP and the Link mesh-networking
   architecture).
3. `MIGRATION-MAP.md` — every Circle subsystem → its Linux replacement, and
   which parts of the codebase are genuinely new rather than ported.
4. `PLAN.md` — what has shipped and how ongoing work is tracked (the original
   pre-build drain order is done; this is now a live-state summary).
5. `DECISIONS.md` — the append-only decision log (rationale + evidence per
   choice).
6. `CONTROLS.md` — the current control-surface reference (loopers, pitch-lock,
   Resonode, granulator, grid-beat visualization).
7. `GLOSSARY.md` — every domain term defined.
8. `BOOT.md`, `RT-TUNING.md` — runtime + tuning detail.

Deeper reference, by topic:
- `COMMAND-SURFACE.md` — the full command/telemetry surface.
- `CLONE-PARITY.md` — parity tracking against the original looper.
- `DEGRADED-MODES.md` — what happens when a subsystem fails or a device is
  missing.
- `HARDWARE-TESTS.md` — the on-hardware test procedures.
- `REMOTE-CONTROL.md` — the UDP remote-control/reboot protocol.
- `LINK-MESH-TESTING.md` — the Link-mesh test plan and current verification
  state.
- `NETBOOT.md`, `FLASHING.md` — image deployment paths (netboot vs SD flash).

**`../AGENTS.md`** (repo root) is the authoritative, continuously-updated
technical-constraints reference — every hardware quirk, DSP gotcha, and
build/deploy failure mode discovered through real-device testing, in far more
byte-level detail than these design docs carry. When a design doc here and
`AGENTS.md` appear to disagree on a technical detail, `AGENTS.md` is more
likely to be current — it is updated every time a new constraint is
discovered, while these docs are updated periodically to keep the higher-level
narrative in sync.
