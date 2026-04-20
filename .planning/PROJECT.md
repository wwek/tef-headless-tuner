# TEF6686 Headless Tuner

## What This Is

ESP32-S3 firmware for a headless TEF6686-based tuner with multiple control interfaces. It exposes USB CDC, USB Audio, Wi-Fi web control, and XDR-GTK TCP control around one unified tuner controller so the same radio state can be driven from serial tools, browsers, and desktop clients.

The current repository is a brownfield codebase: core firmware, transports, and UI already exist, but the project still needs protocol hardening, host-side regression coverage, and real hardware bring-up before it can be treated as a stable v1 release.

## Core Value

Any supported control surface can reliably tune and operate the same TEF6686 radio without protocol drift, transport-specific behavior, or audio/control regressions.

## Requirements

### Validated

- ✓ ESP32-S3 firmware builds with ESP-IDF 5.3.x and produces flashable release artifacts — existing
- ✓ TEF6686 patch loading, clock setup, tuning, seek, RDS readout, and status reads are implemented in a dedicated driver/frontend split — existing
- ✓ USB CDC command handling exists for tune, seek, mute, volume, audio, RDS, status, and identification flows — existing
- ✓ Wi-Fi AP/STA management, REST API endpoints, SSE status streaming, and embedded web pages are present — existing
- ✓ XDR-GTK TCP service exists with authentication, command handling, and state/RDS push flows — existing
- ✓ Host-side Python CLI exists for USB CDC interaction — existing

### Active

- [ ] Stabilize multi-transport control behavior so CDC, Web API, and XDR produce the same tuner outcomes for tune, seek, mute, volume, and status operations
- [ ] Verify protocol compatibility and transport robustness with host-side regression tooling, including fragmented TCP input and XDR state initialization coverage
- [ ] Validate TEF6686 control, audio output, RDS behavior, and seek/squelch behavior on real ESP32-S3 + TEF6686 hardware
- [ ] Ship a release-ready v1 workflow with documentation, CI/release artifacts, and an execution roadmap that reflects the actual remaining work

### Out of Scope

- Internet-facing cloud control or WAN relay features — current scope is local USB/LAN operation only
- Native mobile or desktop GUI apps beyond the embedded web UI and protocol compatibility targets — not required for firmware v1
- Support for unrelated tuner chips or a full multi-device abstraction layer — current codebase is focused on TEF6686/compatible TEF668X usage
- Overbuilding the architecture before hardware validation — real device bring-up must drive any further abstraction work

## Context

- Target platform is ESP32-S3 with native USB device support and ESP-IDF 5.3.1.
- The repository is already structured into `components/tef6686`, `components/tuner_frontend`, and `main` protocol/application layers.
- `.local/` contains upstream/reference projects used for porting and protocol comparison.
- Current repository history shows substantial work already completed on USB, Wi-Fi, web UI, and XDR compatibility.
- Hardware has not arrived yet, so all verification to date is code review plus build-level validation rather than real radio/audio validation.

## Constraints

- **Platform**: Must run on ESP32-S3 under ESP-IDF 5.3.x — this defines available drivers, USB stack behavior, and build tooling.
- **Hardware**: TEF6686 control is over I2C with audio input over I2S — firmware correctness must match the actual module wiring and chip patch version.
- **Verification**: No physical hardware is currently available — protocol and runtime confidence must come from strong code review, build checks, and host-side simulation tooling until bring-up is possible.
- **UX**: Multiple transports must feel consistent — transport-specific quirks are bugs, not acceptable product differences.
- **Scope**: Focus remains on local headless tuner operation — avoid expanding into cloud/mobile/product-adjacent features before v1 stability.

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Keep TEF driver in `components/tef6686` and unified radio service logic in `components/tuner_frontend` | Preserves a clear split between chip-level control and transport-facing behavior | ✓ Good |
| Route CDC, Web, and XDR through a unified tuner controller | Reduces transport drift and keeps radio state changes in one place | ✓ Good |
| Treat this repo as brownfield, not greenfield | Existing firmware and transports already provide validated capabilities that planning should build on | ✓ Good |
| Defer hardware-driven calibration and final confidence to explicit bring-up phases | Real radio/audio validation cannot be faked in software-only review | — Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition**:
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone**:
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-04-20 after initialization*
