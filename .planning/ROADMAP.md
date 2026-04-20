# Roadmap: TEF6686 Headless Tuner

## Overview

This roadmap takes the existing brownfield firmware from “feature-rich but still stabilizing” to a release-ready v1.0. The sequence is intentionally coarse: first harden unified control behavior and protocol compatibility, then add host-side validation and transport parity checks, then finish real hardware bring-up and audio/radio verification, and finally package the project for a confident public release.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [ ] **Phase 1: Control Plane Hardening** - Stabilize shared tuner behavior across CDC, Web, and XDR
- [ ] **Phase 2: Host Validation & Transport Parity** - Add regression tooling and close protocol behavior gaps
- [ ] **Phase 3: Hardware Bring-Up & Audio Verification** - Validate radio and audio behavior on real ESP32-S3 + TEF6686 hardware
- [ ] **Phase 4: Release Readiness** - Finalize release assets, docs, and v1 verification evidence

## Phase Details

### Phase 1: Control Plane Hardening
**Goal**: Make the unified tuner controller and XDR/CDC/Web control surfaces behave consistently for the committed v1 radio-control paths.
**Depends on**: Nothing (first phase)
**Requirements**: PLAT-03, CTRL-01, CTRL-02, CTRL-03, CTRL-04, XDR-01, XDR-02
**Success Criteria** (what must be TRUE):
  1. Tune, seek, band switch, mute, volume, and power commands produce the same observable tuner behavior regardless of whether they come from CDC, Web, or XDR.
  2. XDR clients receive stable init, state, seek, and RDS frames that match the supported protocol contract.
  3. Invalid inputs return explicit transport-appropriate errors instead of silent no-ops or desynchronized state.
**Plans**: 3 plans

Plans:
- [ ] 01-01: Audit and fix controller state consistency across transport adapters
- [ ] 01-02: Finish XDR protocol correctness and response behavior hardening
- [ ] 01-03: Tighten command validation and shared error handling

### Phase 2: Host Validation & Transport Parity
**Goal**: Add host-side regression coverage and verify transport-level behavior under realistic control traffic.
**Depends on**: Phase 1
**Requirements**: WEB-01, WEB-02, WEB-03, XDR-03, HOST-01
**Success Criteria** (what must be TRUE):
  1. A host-side harness can exercise core CDC/XDR command flows and catch protocol regressions without real hardware.
  2. Fragmented TCP input, buffered output pressure, and transport-specific state initialization are regression-tested.
  3. Web REST/SSE behavior and XDR behavior can be compared against the same controller-backed expectations.
**Plans**: 3 plans

Plans:
- [ ] 02-01: Add host-side XDR/CDC regression tooling
- [ ] 02-02: Add automated protocol and transport-parity checks
- [ ] 02-03: Fold host validation into CI-facing verification guidance

### Phase 3: Hardware Bring-Up & Audio Verification
**Goal**: Convert software-only confidence into real-device evidence for tuner control, audio, and RDS behavior.
**Depends on**: Phase 2
**Requirements**: PLAT-02, USB-02, USB-03, HW-01, HW-02, HW-03
**Success Criteria** (what must be TRUE):
  1. Real ESP32-S3 + TEF6686 hardware tunes stations and reports status correctly over I2C.
  2. Real hardware delivers audible audio over the intended path and responds correctly to mute/volume controls.
  3. Real-device testing produces clear pass/fail notes for seek, RDS, and squelch behavior required for release.
**Plans**: 3 plans

Plans:
- [ ] 03-01: Bring up TEF6686 hardware and confirm patch/clock/tune behavior
- [ ] 03-02: Validate audio streaming and transport control on real hardware
- [ ] 03-03: Record real-device verification outcomes and calibration notes

### Phase 4: Release Readiness
**Goal**: Package the project as a maintainable, repeatable v1 release with clear user and contributor guidance.
**Depends on**: Phase 3
**Requirements**: PLAT-01, USB-01
**Success Criteria** (what must be TRUE):
  1. Release artifacts, docs, and repo automation reflect the verified v1 scope rather than aspirational behavior.
  2. The project has a clear release checklist tying together build, protocol, and hardware validation evidence.
  3. Phase completion leaves the project ready for tagged releases and subsequent milestone planning.
**Plans**: 2 plans

Plans:
- [ ] 04-01: Consolidate release docs, constraints, and verification evidence
- [ ] 04-02: Finalize v1 release checklist and packaging workflow

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Control Plane Hardening | 0/3 | Not started | - |
| 2. Host Validation & Transport Parity | 0/3 | Not started | - |
| 3. Hardware Bring-Up & Audio Verification | 0/3 | Not started | - |
| 4. Release Readiness | 0/2 | Not started | - |
