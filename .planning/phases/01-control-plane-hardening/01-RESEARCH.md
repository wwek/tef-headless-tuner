# Phase 1 Research: Control Plane Hardening

**Phase:** 1  
**Generated:** 2026-04-20  
**Source:** Repository state, roadmap, requirements, and current brownfield implementation

## Scope Reminder

Phase 1 is about transport consistency and protocol correctness for existing control paths. It is not the phase to add broader test infrastructure or complete hardware bring-up. Anything that depends on real radio/audio confirmation or a reusable host regression harness should be staged later unless it is strictly necessary to keep Phase 1 changes safe.

## Codebase Facts Relevant to Planning

- Shared radio behavior already flows through `components/tuner_frontend/tuner_controller.c` and `components/tef6686/tef6686.c`.
- CDC command parsing lives in `main/cmd_handler.c`.
- Web REST/SSE control lives in `main/web_server.c`.
- XDR protocol handling lives in `main/xdr_server.c`.
- Current code already contains significant hardening work on XDR buffering, partial input handling, init state, and transport consistency, but that work has not yet been validated through a host-side regression harness.
- Real hardware currently shows TEF I2C failures during boot, which means software-only validation remains the main confidence source for this phase.

## Risks and Planning Implications

### 1. Transport drift risk is mostly about semantics, not raw capability

CDC, Web, and XDR all expose tune/seek/mute/band/volume operations, but the real risk is that one transport interprets state or arguments differently from the others. This means plans should center on:

- One canonical source of tuner state
- One set of validation rules
- One set of response/error semantics per operation family

Do not spread “small fixes” independently across transports without explicitly checking how each operation maps back to `tuner_controller`.

### 2. XDR is the highest-risk transport in Phase 1

The XDR path carries the most protocol nuance:

- Partial input and buffered output
- Authentication handshake
- Legacy client expectations around init/state frames
- Seek completion and raw RDS push behavior

It deserves its own plan so protocol work is isolated from generic controller cleanup.

### 3. Validation without hardware must stay lightweight but real

Phase 1 should not build a full host regression harness yet, but it still needs concrete verification steps. The safest approach is:

- Grep-verifiable plan acceptance criteria
- Build-level validation via `idf.py build`
- Small focused host-side smoke tooling only when it directly supports Phase 1 safety

Anything broader belongs in Phase 2.

### 4. File overlap argues for sequential plans

The roadmap suggests 3 plans. The relevant files overlap heavily:

- `tuner_controller.c/.h`
- `cmd_handler.c`
- `web_server.c`
- `xdr_server.c`

Because state semantics and protocol behavior are tightly coupled, forcing parallel execution would create unnecessary merge/conflict risk. Sequential waves are the safer plan structure for this phase.

### 5. Hardware-specific fixes should stay out unless they unblock control-plane correctness

The current I2C startup failure is real, but Phase 1 should not become a board-bring-up phase. It is acceptable to add diagnostics that make hardware state more visible, but not to broaden into real hardware validation or calibration tasks.

## Recommended Plan Split

### Plan 01-01: Controller and transport contract alignment

Focus:
- Shared state semantics
- Common validation/error handling
- Consistent tune/seek/mute/band behavior across CDC and Web

Why first:
- It reduces the chance that later XDR changes are built on unstable controller assumptions

### Plan 01-02: XDR protocol correctness hardening

Focus:
- XDR init/state semantics
- Authentication and buffering behavior
- Seek/RDS/status compatibility details

Why second:
- XDR has the most nuanced protocol contract and depends on the controller contract being stable

### Plan 01-03: Final transport parity and diagnostics pass

Focus:
- Tighten remaining validation/error paths
- Add startup/runtime diagnostics that make transport mode and access paths obvious
- Close remaining control-plane gaps discovered during the first two plans

Why third:
- This is the cleanup/convergence pass after controller and XDR-specific work are settled

## What Not To Include In Phase 1

- Full host-side XDR/CDC regression harnesses
- Automated fragmented TCP regression suites in CI
- Real hardware tuning/audio verification work
- Broader release-process polish
- New transport surfaces or product features

## Verification Strategy For Phase 1

- Every plan should end with `idf.py build`
- Acceptance criteria should be grep-able and tied to concrete strings, fields, or behaviors in the touched files
- Plans should explicitly list exact files to read first so executors do not guess current signatures or conventions
- The phase-level must-haves should focus on:
  - Shared control consistency
  - Protocol-compatible XDR behavior
  - Explicit and actionable error handling

## Planning Recommendation

Use **3 sequential plans (waves 1 → 2 → 3)**:

- `01-01` wave 1, no dependencies
- `01-02` wave 2, depends on `01-01`
- `01-03` wave 3, depends on `01-01` and `01-02`

This keeps overlapping files safe while still making progress through coherent work slices.

## Research Complete

Phase 1 should stay narrowly focused on shared control behavior and XDR protocol correctness. The safest plan shape is three sequential plans: controller contract alignment, XDR hardening, then parity/diagnostics cleanup.
