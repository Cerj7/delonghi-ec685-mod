# Tasks: Descaling Program (EC685)

**Input**: Design documents from `/specs/002-descaling-program/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/, quickstart.md

**Tests**: Manual stand validation only (per research.md / quickstart.md). No automated unit/contract test tasks.

**Organization**: Tasks are grouped by user story. Delta scope: `scripts/011_button_panel_ui.cpp` (`R3`), `scripts/013_main_brew_logic.cpp` (hold-enter, descale FSM, OLED, heater gate, `DC0` after phase 2), `docs/button-panel-protocol.md`, `docs/main-brew-logic.md`. Reuse `001` DecalcTimer / `handleServiceDc0()` / PI heater / flow ISR — do not change EEPROM layout.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Primary deliverables: `scripts/011_button_panel_ui.cpp`, `scripts/013_main_brew_logic.cpp`
- Docs: `docs/button-panel-protocol.md`, `docs/main-brew-logic.md`
- Feature artifacts: `specs/002-descaling-program/`
- References (read-only): `specs/001-main-brew-logic/`, `docs/pic16f1938-operating-modes.md`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Align Tunables / docs stubs and inventory existing `001` hooks before FSM work

- [X] T001 Add full `DESCALE_*` Tunables block at top of `scripts/013_main_brew_logic.cpp` per `specs/002-descaling-program/contracts/descale-tunables.md` (`DESCALE_HOLD_PROMPT_MS=1000`, `DESCALE_HOLD_ENTER_MS=10000`, `DESCALE_HOLD_ENTER_TOL_MS=200`, `DESCALE_PHASE_VOLUME_ML=1000`, `DESCALE_PHASE1_ON_MS=500`, `DESCALE_PHASE1_OFF_MS=1000`, `DESCALE_PHASE1_NOFLOW_MS=1000`, `DESCALE_PHASE2_ON_MS=500`, `DESCALE_PHASE2_OFF_MS=500`, `DESCALE_PHASE2_NOFLOW_MS=1500`, `DESCALE_PHASE1_TEMP_C=53`, `DESCALE_PUMP_PCT=100`, `DESCALE_PULSE_TOL_MS=50`, `DESCALE_DC0_MAX_LATENCY_MS=1000`) with FR/SC comments — zero business magic literals in later body code; keep requiring `FLOW_PULSES_PER_ML > 0` for volume scenarios
- [X] T002 [P] Draft / extend stub sections in `docs/main-brew-logic.md` for descaling program (hold-enter, FSM states pointer, heater policy 53 °C phase1 / 0 % elsewhere, link to `specs/002-descaling-program/`)
- [X] T003 [P] Draft / extend stub note in `docs/button-panel-protocol.md` that «пар» will use press `B3` + release `R3` (pair-firmware requirement FR-018a) — full table update lands in polish

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Panel release code, main parse/`MachineState` shell, session/hold models, heater/pump gate hooks — MUST complete before any user story

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Implement confirmed release of «пар» → UART `R3` in `scripts/011_button_panel_ui.cpp` per `specs/002-descaling-program/contracts/panel-hold-uart.md` (debounce press/release local; `B1`/`B2` without required release codes; panel MUST NOT measure 1 s / 10 s or send `H3`/`D3`)
- [X] T005 Extend panel UART RX parse in `scripts/013_main_brew_logic.cpp` to accept `R3` alongside `B1`/`B2`/`B3` (≤`LINK_CODE_MAX_LEN`); `R3` without active hold-gesture = no-op; outside hold/descale keep `001` `B3` matrix
- [X] T006 Extend `MachineState` enum in `scripts/013_main_brew_logic.cpp` with `DESCALE_HOLD`, `DESCALE_PREP`, `DESCALE_PHASE1`, `DESCALE_REFILL`, `DESCALE_WAIT_RINSE`, `DESCALE_PHASE2`, `DESCALE_REFILL_WATER` per `specs/002-descaling-program/data-model.md` and `contracts/descale-fsm.md`; add dispatch stubs in `onPanelButton` / `updateMachineState` / `loop`
- [X] T007 Add RAM-only `DescaleHoldGesture` (`pressMs`, `active`, `countdownVisible`) and `DescaleSession` (`phase` NONE|PHASE1|PHASE2, `phasePulses`, derived `phaseVolumeMl`, `pulsePhase` ON|OFF, `pulsePhaseStartedMs`, `lastFlowPulseMs`, `noFlowArmed`) in `scripts/013_main_brew_logic.cpp` per data-model — no EEPROM checkpoint of phase (power cycle does not resume)
- [X] T008 Implement shared descale heater/pump gates in `scripts/013_main_brew_logic.cpp`: `descaleHeaterSetpoint()` / force-off for PREP/REFILL/WAIT_RINSE/PHASE2/REFILL_WATER; setpoint `DESCALE_PHASE1_TEMP_C` only in `DESCALE_PHASE1`; `DESCALE_HOLD` keeps espresso PI; extend `pumpMayRun()` true only in PHASE1/PHASE2 during pulse ON; never use espresso/steam setpoint inside `DESCALE_*` (except hold-countdown espresso path)
- [X] T009 [P] Add PROGMEM/`F()` Ukrainian OLED prompt IDs/strings skeleton in `scripts/013_main_brew_logic.cpp` for COUNTDOWN / PREP / HANDLE_CLOSED / PHASE1_RUN / REFILL / WAIT_RINSE / PHASE2_RUN / REFILL_WATER per data-model `DescaleUiPrompt` (shorten only if SSD1306 overflow; meaning MUST stay unambiguous — FR-017)
- [X] T010 Wire FAULT / invalid thermocouple / safety cutoff from any `DESCALE_*` in `scripts/013_main_brew_logic.cpp`: heater OFF, pump `PUMP_OFF_PCT`, enter `FAULT`, **no** `DC0` / no DecalcTimer clear (FR-014)

**Checkpoint**: Foundation ready — `R3` on wire, enum/session/gates compile; user stories can start

---

## Phase 3: User Story 1 - Вхід у програму декальцинації (Priority: P1) 🎯 MVP

**Goal**: Hold «пар» 10 s (with or without `DC1`) → OLED countdown after 1 s → `DESCALE_PREP`; new `B3` + open handle starts phase 1 with 53 °C setpoint and immediate pulses stub/entry

**Independent Test**: quickstart S1–S3 — short press <1 s = steam path; release during 1…10 s cancels; hold ≥10 s (±200 ms) enters prep with/without `DC1`; closed handle blocks start; new press with open handle → phase 1 UI + heater setpoint 53 °C, pump may start (full pulse/volume logic → US2)

### Implementation for User Story 1

- [X] T011 [US1] On `B3` from `ESPRESSO_READY` / `ESPRESSO_HEAT` in `scripts/013_main_brew_logic.cpp`: enter `DESCALE_HOLD`, record `pressMs`, do **not** call `enterSteam()` immediately; ignore when already in brew/steam/`FAULT` (FR-001, FR-002, FR-018)
- [X] T012 [US1] In `DESCALE_HOLD` tick of `scripts/013_main_brew_logic.cpp`: after `DESCALE_HOLD_PROMPT_MS` show OLED countdown «Режим декальцинації через N сек» updating while held; espresso heater PI remains (FR-003, FR-006a countdown); on `R3` with elapsed < prompt → short steam path via `enterSteam()` / `001` matrix; on `R3` with prompt ≤ elapsed < enter → cancel countdown, restore ready/heat, no steam, no program
- [X] T013 [US1] When hold elapsed ≥ `DESCALE_HOLD_ENTER_MS` in `scripts/013_main_brew_logic.cpp`: transition to `DESCALE_PREP` regardless of `dcLatched`/`DC1`; pump `PUMP_OFF_PCT`; heater force-off; OLED PREP «Залийте 1 л очисника, відкрийте ручку та натисніть кнопку "Пар"»; `R3` after prep entry MUST NOT cancel program (FR-001, FR-004, FR-004a)
- [X] T014 [US1] In `DESCALE_PREP` of `scripts/013_main_brew_logic.cpp`: require **new** `B3` front + `isSteamHandleOpen()` to enter `DESCALE_PHASE1`; sticky hold without release MUST NOT start pump; closed handle + `B3` → stay + OLED HANDLE_CLOSED; ignore `B1`/`B2`; ignore `R3` (FR-004a, FR-005, FR-015)
- [X] T015 [US1] On entry to `DESCALE_PHASE1` from prep in `scripts/013_main_brew_logic.cpp`: OLED PHASE1_RUN «Виконується декальцинація: фаза очистки»; set heater setpoint `DESCALE_PHASE1_TEMP_C` (53); start pulse path immediately with no temperature-gate (FR-006, FR-006a); optionally neutralize brew/steam LEDs (`SS0`/`HS0`/`LT0`) once at prep entry if needed so `DC1` remains visible
- [X] T016 [US1] Ensure short «пар» without hold ≥ prompt still uses existing steam logic in `scripts/013_main_brew_logic.cpp` with inactive or active `DC1` alike (SC-001, FR-001 edge)

**Checkpoint**: US1 alone — hold-enter + prep + gated start into phase 1 with 53 °C policy; short vs hold correct on paired firmwares

---

## Phase 4: User Story 2 - Фаза 1: очистка імпульсною помпою (Priority: P1)

**Goal**: Phase 1 pulse pump 500/1000 ms @ 100 %, volume to `DESCALE_PHASE_VOLUME_ML`, early no-flow → refill cleaner (heater 0 %), complete → wait-rinse without clearing `DC1`

**Independent Test**: quickstart S4 — timings ±50 ms; setpoint ~53 °C during phase 1; early empty → refill OLED + resume volume; target reached → wait-rinse; abort leaves `DC1`

### Implementation for User Story 2

- [X] T017 [US2] Implement phase-1 pulse driver in `scripts/013_main_brew_logic.cpp`: ON `DESCALE_PHASE1_ON_MS` @ `DESCALE_PUMP_PCT` / OFF `DESCALE_PHASE1_OFF_MS` @ `PUMP_OFF_PCT` via `millis()` (no `delay()`); PI holds `DESCALE_PHASE1_TEMP_C` through ON and OFF segments (FR-007, FR-006)
- [X] T018 [US2] Accumulate `phasePulses` from flow ISR/counter into `DescaleSession` in `scripts/013_main_brew_logic.cpp`; derive ml via `FLOW_PULSES_PER_ML`; compare to `DESCALE_PHASE_VOLUME_ML` (FR-009); on fresh phase-1 entry zero pulses; on resume from refill keep pulses
- [X] T019 [US2] Arm no-flow only during pulse ON in `scripts/013_main_brew_logic.cpp`: if `(now - lastFlowPulseMs) ≥ DESCALE_PHASE1_NOFLOW_MS` and volume < target → `DESCALE_REFILL` (pump 0, heater 0 %, OLED REFILL «Долийте очисник і натисніть кнопку "Пар"», keep `phasePulses`, keep `DC1`); pause/reset no-flow timer on OFF (FR-010)
- [X] T020 [US2] Complete phase 1 in `scripts/013_main_brew_logic.cpp` when volume ≥ `DESCALE_PHASE_VOLUME_ML` **or** no-flow ON with volume already ≥ target (priority over refill): pump 0, heater 0 %, → `DESCALE_WAIT_RINSE`, OLED WAIT_RINSE «Залийте 1 л води та натисніть кнопку "Пар"», do **not** send `DC0` / do not clear DecalcTimer (FR-011, FR-014)
- [X] T021 [US2] In `DESCALE_REFILL` of `scripts/013_main_brew_logic.cpp`: new `B3` + open handle → resume `DESCALE_PHASE1` (pulses + 53 °C again, volume kept); closed handle → HANDLE_CLOSED; ignore `B1`/`B2`/`R3`; no auto-timeout abort (FR-014a)

**Checkpoint**: US1+US2 — full clean phase with refill/complete; reminder still latched

---

## Phase 5: User Story 3 - Фаза 2: промивання та скидання нагадування (Priority: P1)

**Goal**: Wait-rinse → phase 2 pulses 500/500, heater 0 %, early no-flow → refill water without `DC0`; success volume ≥ target → `handleServiceDc0()` effect and return to espresso path

**Independent Test**: quickstart S5 (+ S7/S8 negatives) — closed handle blocks; early empty → water refill no `DC0`; success → `DC0` ≤1 s + latch/interval clear; abort/FAULT/power-loss → no `DC0`

### Implementation for User Story 3

- [X] T022 [US3] In `DESCALE_WAIT_RINSE` of `scripts/013_main_brew_logic.cpp`: heater/pump stay 0 %; new `B3` + open handle → `DESCALE_PHASE2` (zero `phasePulses` for new phase); closed handle → HANDLE_CLOSED; ignore `B1`/`B2`/`R3`; no auto-timeout (FR-005, FR-014a)
- [X] T023 [US3] Implement phase-2 pulse driver in `scripts/013_main_brew_logic.cpp`: ON `DESCALE_PHASE2_ON_MS` / OFF `DESCALE_PHASE2_OFF_MS` @ `DESCALE_PUMP_PCT` / `PUMP_OFF_PCT`; heater force-off entire phase 2 (FR-008, FR-006); OLED PHASE2_RUN
- [X] T024 [US3] Phase-2 volume + no-flow in `scripts/013_main_brew_logic.cpp`: no-flow ON ≥ `DESCALE_PHASE2_NOFLOW_MS` and volume < target → `DESCALE_REFILL_WATER` (pump 0, OLED REFILL_WATER «Долийте воду і натисніть кнопку "Пар"», keep pulses, **no** `DC0`, keep `DC1`); volume ≥ target or no-flow with volume ≥ target → success path (FR-012, FR-012a)
- [X] T025 [US3] In `DESCALE_REFILL_WATER` of `scripts/013_main_brew_logic.cpp`: new `B3` + open handle → resume `DESCALE_PHASE2` (volume kept, heater 0 %); closed handle → HANDLE_CLOSED; ignore `B1`/`B2`/`R3`; no auto-timeout (FR-012, FR-014a)
- [X] T026 [US3] On successful phase-2 complete in `scripts/013_main_brew_logic.cpp`: pump 0; call shared clear identical to USB path — `sendToPanel("DC0")` + `dcLatched=false` + `accumulatedOnSeconds=0` + EEPROM save via existing `handleServiceDc0()` (or extracted shared helper); within `DESCALE_DC0_MAX_LATENCY_MS`; then `coolOrReady()` / espresso heat-or-ready path — not steam target (FR-013, SC-005)
- [X] T027 [US3] Confirm USB `DC0` in `scripts/013_main_brew_logic.cpp` still clears reminder without running the program (FR-019); interrupted phase 1/2 / FAULT / power cycle never auto-sends `DC0` or clears interval (FR-014)

**Checkpoint**: Full clean→rinse→`DC0` path works; negatives preserve `DC1`

---

## Phase 6: User Story 4 - Потік через панарело та група (Priority: P2)

**Goal**: Closing steam handle mid-phase does not auto-complete the program; pulses continue until volume/no-flow/refill; brew buttons stay ignored

**Independent Test**: quickstart S6 — open handle → pannarello path expected; close mid-phase → group path hydraulically; FSM does not treat handle close as phase end; `B1`/`B2` ignored

### Implementation for User Story 4

- [X] T028 [US4] In `DESCALE_PHASE1` / `DESCALE_PHASE2` of `scripts/013_main_brew_logic.cpp`: closing `isSteamHandleOpen()` mid-phase MUST NOT stop pulses, MUST NOT transition to wait/success/refill by itself — only volume / no-flow / FAULT rules apply (US4, FR edge)
- [X] T029 [US4] Audit button matrix for all `DESCALE_*` in `scripts/013_main_brew_logic.cpp` per `contracts/descale-fsm.md`: `B1`/`B2` ignored (no cold flush / espresso); `B3`/`R3` rules match state table; document hydraulic path expectation (pannarello vs group) in `docs/main-brew-logic.md` without claiming solenoid control

**Checkpoint**: Mid-phase handle close is hydraulic-only; UI/FSM stable

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Docs sync, magic-literal compliance, paired-firmware quickstart pass

- [X] T030 [P] Finalize `docs/button-panel-protocol.md`: «пар» press+release (`B3`/`R3`), compatibility table (old panel unsupported for `002` hold-enter; new panel + old main ignores `R3`), pair-flash requirement FR-018a / SC-008
- [X] T031 [P] Finalize `docs/main-brew-logic.md` descaling section: FSM overview, heater policy (53 °C only `DESCALE_PHASE1`), pulse/no-flow/refill symmetry, success=`DC0`, no phase EEPROM resume, Tunables pointer, link `specs/002-descaling-program/`
- [ ] T032 Run SC-007 compliance sweep: `rg` on `scripts/013_main_brew_logic.cpp` for descale business literals (`10000`, `1000`, `500`, `1500`, `53`) outside Tunables; ensure no `STEAM_PULSE_*` / espresso-steam setpoints inside descale phases; heater gate matches contracts
- [ ] T033 Execute `specs/002-descaling-program/quickstart.md` scenarios S1–S8 on stand with **paired** `011`+`013` firmwares; temporarily reduce Tunables only inside Tunables section if needed; record pass/fail against SC-001…SC-008

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Setup — **BLOCKS** all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational — MVP
- **User Story 2 (Phase 4)**: Depends on US1 entry into `DESCALE_PHASE1` (heater/session shell)
- **User Story 3 (Phase 5)**: Depends on US2 reaching `DESCALE_WAIT_RINSE`
- **User Story 4 (Phase 6)**: Depends on US2/US3 pulse states existing; can harden matrix after pulses land
- **Polish (Phase 7)**: Depends on desired stories complete

### User Story Dependencies

- **User Story 1 (P1)**: After Foundational — no dependency on US2–US4
- **User Story 2 (P1)**: After US1 phase-1 entry hook (T015)
- **User Story 3 (P1)**: After US2 wait-rinse transition (T020)
- **User Story 4 (P2)**: After phase pulse states (US2/US3); independently testable on stand via S6

### Within Each User Story

- Models/gates (Foundational) before story FSM behavior
- Entry/UI before pulse/volume
- Phase complete / refill before next-phase start
- Success `DC0` only after phase-2 volume criteria
- Story checkpoint before raising priority scope

### Parallel Opportunities

- T002 // T003 (docs stubs) after T001 Tunables landed or in parallel with different files
- T004 (`011` `R3`) // T009 (OLED strings) while T005–T008 proceed carefully on `013` (same-file sequential preferred for T005–T008)
- T030 // T031 docs finalize in polish
- Single-dev reality: most `013` tasks are sequential; use [P] only across `011` vs `013` vs docs

---

## Parallel Example: Foundational

```bash
# Different files — safe together:
Task: "Implement R3 release in scripts/011_button_panel_ui.cpp"
Task: "Add PROGMEM OLED descale prompts skeleton in scripts/013_main_brew_logic.cpp"
Task: "Draft docs/button-panel-protocol.md R3 stub"

# Same file — run sequentially on 013:
Task: "Parse R3 on main"
Task: "Extend MachineState enum + stubs"
Task: "Add DescaleHoldGesture / DescaleSession"
Task: "Heater/pump descale gates"
```

## Parallel Example: User Story 1

```bash
# After foundational — typically one implementer on 013:
Task: "B3 → DESCALE_HOLD without enterSteam()"
Task: "Countdown + R3 short/cancel paths"
Task: "Hold ≥ enter → DESCALE_PREP"
# Then:
Task: "PREP new B3 + handle → PHASE1"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (Tunables + doc stubs)
2. Complete Phase 2: Foundational (`R3`, enum, session, gates) — CRITICAL
3. Complete Phase 3: User Story 1 (hold-enter → prep → gated phase-1 entry)
4. **STOP and VALIDATE**: quickstart S1–S3 on paired firmwares
5. Flash/demo if ready

### Incremental Delivery

1. Setup + Foundational → `R3` + shell ready
2. US1 → hold/prep/start validated (MVP)
3. US2 → clean phase + refill
4. US3 → rinse + `DC0` success path
5. US4 → mid-phase handle / matrix audit
6. Polish → docs + SC-007 + full quickstart S1–S8

### Parallel Team Strategy

With two developers:

1. Together: Phase 1–2 (`011` owner takes T004; `013` owner takes T005–T010)
2. Then:
   - Dev A: US1 → US2 on `013`
   - Dev B: docs T030/T031 + stand harness notes; join US3/US4 when pulses exist
3. Together: T032/T033 compliance + stand

---

## Notes

- [P] tasks = different files, no incomplete dependencies
- [Story] label maps task to US1–US4 for traceability
- Tests = manual quickstart only (no automated suite requested)
- Both firmwares MUST ship together (FR-018a); mixed old-panel/new-main out of scope
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid magic literals outside Tunables; reuse `handleServiceDc0()` for success clear
)
