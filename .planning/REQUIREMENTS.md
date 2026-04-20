# Requirements: TEF6686 Headless Tuner

**Defined:** 2026-04-20
**Core Value:** Any supported control surface can reliably tune and operate the same TEF6686 radio without protocol drift, transport-specific behavior, or audio/control regressions.

## v1 Requirements

### Platform

- [ ] **PLAT-01**: Firmware builds for `esp32s3` with ESP-IDF 5.3.x and produces flashable release artifacts
- [ ] **PLAT-02**: Device initializes TEF6686 with the selected patch and crystal configuration and reaches an operational tuner state
- [ ] **PLAT-03**: Runtime state remains consistent across transports and can be queried without desynchronizing control logic

### Radio Control

- [ ] **CTRL-01**: User can tune FM, LW, MW, and SW frequencies through the unified tuner controller
- [ ] **CTRL-02**: User can start and stop seek operations for both FM and AM bands from supported control surfaces
- [ ] **CTRL-03**: User can mute, unmute, change volume, switch bands, and power the tuner on or off
- [ ] **CTRL-04**: Invalid control input is rejected with explicit, actionable errors instead of silent failure

### USB & Audio

- [ ] **USB-01**: Device enumerates as the intended USB composite device for control and audio on supported hosts
- [ ] **USB-02**: Host receives TEF6686 audio over the configured I2S-to-USB audio path
- [ ] **USB-03**: USB-side mute/volume behavior is reflected in the streamed audio output

### Web & Wi-Fi

- [ ] **WEB-01**: User can connect over AP or STA mode and open the embedded web control UI
- [ ] **WEB-02**: REST endpoints expose tune, seek, mute, status, quality, RDS, and Wi-Fi configuration flows
- [ ] **WEB-03**: SSE updates stream live status, quality, and RDS changes without polling the full UI

### XDR & Protocols

- [ ] **XDR-01**: XDR-GTK TCP clients can authenticate and issue the supported control command set successfully
- [ ] **XDR-02**: XDR state push, seek completion, and raw RDS frames follow protocol-compatible formatting
- [ ] **XDR-03**: TCP command handling remains correct under partial/fragmented input and buffered output pressure
- [ ] **HOST-01**: Host-side control tooling can exercise at least the primary CDC and XDR control paths for regression checking

### Hardware Verification

- [ ] **HW-01**: Real ESP32-S3 + TEF6686 hardware can tune stations and report status correctly over I2C
- [ ] **HW-02**: Real hardware produces audible output over the configured audio path
- [ ] **HW-03**: Real hardware confirms seek, RDS, and squelch behavior is acceptable for release

## v2 Requirements

### Future Extensions

- **FUT-01**: Add automated end-to-end protocol regression coverage beyond the initial host harness
- **FUT-02**: Add richer diagnostics and calibration utilities for field debugging
- **FUT-03**: Evaluate broader TEF668X compatibility once TEF6686 v1 behavior is stable

## Out of Scope

| Feature | Reason |
|---------|--------|
| Internet/cloud remote control | Local headless tuner control is the v1 scope |
| Native mobile apps | Web UI and protocol clients are sufficient for current goals |
| Non-TEF tuner abstraction layer | Premature expansion before TEF6686 hardware validation |
| Feature expansion beyond stable radio control/audio/protocol compatibility | v1 should prioritize correctness over breadth |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| PLAT-01 | Phase 4 | Pending |
| PLAT-02 | Phase 3 | Pending |
| PLAT-03 | Phase 1 | Pending |
| CTRL-01 | Phase 1 | Pending |
| CTRL-02 | Phase 1 | Pending |
| CTRL-03 | Phase 1 | Pending |
| CTRL-04 | Phase 1 | Pending |
| USB-01 | Phase 4 | Pending |
| USB-02 | Phase 3 | Pending |
| USB-03 | Phase 3 | Pending |
| WEB-01 | Phase 2 | Pending |
| WEB-02 | Phase 2 | Pending |
| WEB-03 | Phase 2 | Pending |
| XDR-01 | Phase 1 | Pending |
| XDR-02 | Phase 1 | Pending |
| XDR-03 | Phase 2 | Pending |
| HOST-01 | Phase 2 | Pending |
| HW-01 | Phase 3 | Pending |
| HW-02 | Phase 3 | Pending |
| HW-03 | Phase 3 | Pending |

**Coverage:**
- v1 requirements: 20 total
- Mapped to phases: 20
- Unmapped: 0 ✓

---
*Requirements defined: 2026-04-20*
*Last updated: 2026-04-20 after initial definition*
