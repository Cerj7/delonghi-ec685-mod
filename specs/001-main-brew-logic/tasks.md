# Tasks: Main Brew Logic (EC685)

**Input**: Design documents from `/specs/001-main-brew-logic/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/, quickstart.md

**Tests**: Manual stand validation only (per research.md / quickstart.md). No automated unit/contract test tasks.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Primary deliverable: `scripts/013_main_brew_logic.cpp` (monolithic sketch)
- Docs: `docs/main-brew-logic.md`, feature artifacts under `specs/001-main-brew-logic/`
- References (read-only): `scripts/006_thermoblock_pi_heater.cpp`, `scripts/012_main_panel_link.cpp`, `docs/button-panel-protocol.md`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the new main-board sketch skeleton and Tunables section from plan / contracts

- [X] T001 Create `scripts/013_main_brew_logic.cpp` by copying structure from `scripts/006_thermoblock_pi_heater.cpp` (header comment, includes: Wire/GFX/SSD1306/MAX6675/AC_Dimmer/avr/wdt/math/stdio/string + SoftwareSerial + EEPROM)
- [X] T002 Replace D8/D9 button stubs with panel UART pins (`PANEL_RX_PIN=9`, `PANEL_TX_PIN=8`, `LINK_BAUD=19200`, `PC_BAUD=115200`) per `specs/001-main-brew-logic/contracts/panel-uart.md` and `scripts/012_main_panel_link.cpp` in `scripts/013_main_brew_logic.cpp`
- [X] T003 Add full Tunables section at top of `scripts/013_main_brew_logic.cpp` with every constant from `specs/001-main-brew-logic/contracts/tunable-constants.md` (temperatures including `STEAM_TARGET_C=120`, `SAFETY_CUTOFF_C=130`, ready/brew/steam timings, powers, PI, decalc, link, `FLOW_PULSES_PER_ML`) and FR/SC comments — zero business magic literals in later body code
- [X] T004 [P] Draft stub outline for `docs/main-brew-logic.md` (purpose, pinout deltas vs `006`, Tunables pointer, link to `specs/001-main-brew-logic/`)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core heater, panel link, FSM shell, I/O, and safety that MUST exist before any user story

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 Port thermocouple read, pot EMA, PI heater window PWM, dimmer curve, OLED idle layout, watchdog, and unused-pin setup from `scripts/006_thermoblock_pi_heater.cpp` into `scripts/013_main_brew_logic.cpp` using Tunables (`SAFETY_CUTOFF_C` raised for steam)
- [X] T006 Implement panel UART framing (`sendToPanel`, RX line buffer ≤`LINK_CODE_MAX_LEN`, parse `B1`/`B2`/`B3`) in `scripts/013_main_brew_logic.cpp` per `specs/001-main-brew-logic/contracts/panel-uart.md`
- [X] T007 [P] Implement USB Serial service parser skeleton in `scripts/013_main_brew_logic.cpp` (accept `DC0`; optionally log unknown; leave DC handlers stubbed until US5)
- [X] T008 Define `MachineState` enum (`ESPRESSO_HEAT`, `ESPRESSO_READY`, `BREW_1`, `BREW_2`, `STEAM_HEAT`, `STEAM_READY`, `STEAM_FLOW`, `STEAM_COOL`, `FAULT` — без `COLD_START`) and central `onPanelButton` / `loop` dispatch stubs in `scripts/013_main_brew_logic.cpp` per `specs/001-main-brew-logic/data-model.md` and `contracts/main-fsm.md`
- [X] T009 Implement shared helpers in `scripts/013_main_brew_logic.cpp`: `espressoTargetC` from pot, `activeTargetC` mode ESPRESSO|STEAM, ready criterion (`READY_BAND_C` + `READY_HOLD_MS`), `setPumpPercent` / pump-authority default `PUMP_OFF_PCT` outside brew/steam flow, `enterFault` (heater OFF, pump 0, OLED fault)
- [X] T010 [P] Wire inputs in `scripts/013_main_brew_logic.cpp`: `STEAM_HANDLE_PIN` (D10) read API; `FLOWMETER_PIN` (D5) pulse counter ISR/poll with reset-on-brew-start hook (display units later)
- [X] T011 Implement `coolOrReady()` in `scripts/013_main_brew_logic.cpp` per `contracts/main-fsm.md` (target→espresso pot; if hotter → `LT1`/`STEAM_COOL`; else `LT0` then `HE1`/`ESPRESSO_HEAT` or `ESPRESSO_READY`)

**Checkpoint**: Foundation ready — heater, link, FSM shell, I/O, and helpers compile; user stories can start

---

## Phase 3: User Story 1 - Увімкнення та розігрів до готовності (Priority: P1) 🎯 MVP

**Goal**: Power-on espresso heat with `HE1`/`HE0`, ignore all buttons until ready

**Independent Test**: Power board with temp pot set; ≤1 s see `HE1`; buttons do not start pump/steam; after `|Tt−target|≤READY_BAND_C` for `READY_HOLD_MS` → `HE0` (quickstart S1)

### Implementation for User Story 1

- [X] T012 [US1] On `setup`/power-on in `scripts/013_main_brew_logic.cpp`: enter `ESPRESSO_HEAT`, set `activeTargetC` from espresso pot, send `HE1` immediately (FR-001, FR-002)
- [X] T013 [US1] In `ESPRESSO_HEAT` loop path of `scripts/013_main_brew_logic.cpp`: run PI to pot target; when ready criterion holds continuously → send `HE0`, transition `ESPRESSO_READY` (FR-002, SC-001)
- [X] T014 [US1] In button matrix for `ESPRESSO_HEAT` (and `FAULT`) in `scripts/013_main_brew_logic.cpp`: ignore `B1`/`B2`/`B3` with no pump/mode/indication side effects (FR-003)
- [X] T015 [US1] Keep pump at `PUMP_OFF_PCT` throughout heat/ready idle in `scripts/013_main_brew_logic.cpp` regardless of pump pot (FR-019)
- [X] T016 [US1] Ensure fault path in `scripts/013_main_brew_logic.cpp` latches `FAULT` (heater OFF, pump 0, OLED fault) and stays until **power cycle** — no auto-recovery (FR-018)

**Checkpoint**: US1 alone — heat → ready → buttons ignored until `HE0`

---

## Phase 4: User Story 2 - Приготування 1 еспресо з преінфузією (Priority: P1)

**Goal**: From ready, `B1` runs preinfusion/pause/flow with OLED brew metrics; same button stops

**Independent Test**: After `HE0`, press 1 espresso; verify 2 s @ 40 % → 2 s pause → pot power; OLED ≥2 Hz; second `B1` stops ≤300 ms with `P10`; `B2`/`B3` ignored (quickstart S2)

### Implementation for User Story 2

- [X] T017 [US2] On `B1` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `P11`, enter `BREW_1`, reset water counter, start `PREINFUSE` phase (FR-004)
- [X] T018 [US2] Implement brew phase machine for `BREW_1` in `scripts/013_main_brew_logic.cpp`: `PREINFUSION_MS` @ `PREINFUSION_PCT` → `PREINFUSION_PAUSE_MS` @ `PUMP_OFF_PCT` → `FLOW` at live pump pot (ignore pot during preinfusion) (FR-004, SC-002)
- [X] T019 [US2] On repeat `B1` in `BREW_1` in `scripts/013_main_brew_logic.cpp`: pump `PUMP_OFF_PCT`, send `P10`, return `ESPRESSO_READY` within `BUTTON_REACT_MAX_MS` budget (FR-005, SC-003)
- [X] T020 [US2] Ignore `B2`/`B3` during `BREW_1` in `scripts/013_main_brew_logic.cpp` (FR-005)
- [X] T021 [US2] During `BREW_1` OLED path in `scripts/013_main_brew_logic.cpp`: every `BREW_DISPLAY_INTERVAL_MS` show water counter (pulses or ml via `FLOW_PULSES_PER_ML`), block temp, pump % (FR-008, SC-004)
- [X] T022 [US2] Keep espresso PI target from temp pot during brew in `scripts/013_main_brew_logic.cpp`; on fault abort brew (pump 0, no dangling `P11`)

**Checkpoint**: US1+US2 — single-shot brew works end-to-end from ready

---

## Phase 5: User Story 3 - Приготування 2 еспресо з преінфузією (Priority: P1)

**Goal**: Same brew profile as US2 with `B2` / `P21` / `P20`

**Independent Test**: From ready, `B2` → same timing/power sequence → stop with `B2`/`P20`; `B1`/`B3` ignored (quickstart S3)

### Implementation for User Story 3

- [X] T023 [P] [US3] On `B2` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `P21`, enter `BREW_2`, reset counter, reuse shared brew phase machine (FR-006)
- [X] T024 [US3] On repeat `B2` in `BREW_2` in `scripts/013_main_brew_logic.cpp`: pump off, send `P20`, return `ESPRESSO_READY`; ignore `B1`/`B3` during `BREW_2` (FR-007)
- [X] T025 [US3] Confirm `BREW_2` uses same OLED brew display path as `BREW_1` in `scripts/013_main_brew_logic.cpp` (FR-008)

**Checkpoint**: Both shot modes independently stoppable; foreign buttons ignored

---

## Phase 6: User Story 4 - Режим пару (Priority: P2)

**Goal**: Steam heat to 120 °C, handle-driven pulses, idle/`B3` cancel via cool path; ignore espresso buttons in steam/cool

**Independent Test**: From ready run quickstart S4 (`HS1`→`HS0`, cancel, handle `SS1`/`SS0`, idle 3 s → `LT*`, `B3`@`HS0` cancel, ignore `B1`/`B2`)

### Implementation for User Story 4

- [X] T026 [US4] On `B3` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `HS1`, enter `STEAM_HEAT`, `activeTargetC=STEAM_TARGET_C` (FR-009, FR-013)
- [X] T027 [US4] In `STEAM_HEAT` of `scripts/013_main_brew_logic.cpp`: heat to steam target; on `|Tt−STEAM_TARGET_C|≤STEAM_READY_BAND_C` send `HS0` immediately (**no** `READY_HOLD_MS`) and enter `STEAM_READY` (FR-011, SC-005)
- [X] T028 [US4] On `B3` during `STEAM_HEAT` in `scripts/013_main_brew_logic.cpp`: cancel via `coolOrReady()` (FR-010)
- [X] T029 [US4] In `STEAM_READY` of `scripts/013_main_brew_logic.cpp`: open handle → `SS1`/`STEAM_FLOW`; closed idle `STEAM_IDLE_EXIT_MS` → `LT1`/`STEAM_COOL`; `B3` → `coolOrReady()` without `SS1` (FR-012, FR-014, FR-021, SC-006/007)
- [X] T030 [US4] Implement steam pulse pump in `STEAM_FLOW` of `scripts/013_main_brew_logic.cpp`: `STEAM_PULSE_ON_MS`/`STEAM_PULSE_OFF_MS` @ `STEAM_PULSE_PCT`; handle close → `SS0`, pump 0, then `coolOrReady()` (not idle `HS0` / 3 s) (FR-012, FR-015, SC-006)
- [X] T031 [US4] Complete `STEAM_COOL` in `scripts/013_main_brew_logic.cpp`: target espresso pot; ready criterion → `LT0` → `ESPRESSO_READY`; ignore `B1`/`B2` in all steam/cool states; ignore `B3` in `STEAM_FLOW`/`STEAM_COOL` (FR-014, FR-022)
- [X] T032 [US4] Hold steam PI near `STEAM_TARGET_C` until exit and keep `SAFETY_CUTOFF_C` protective in `scripts/013_main_brew_logic.cpp` (FR-013, FR-018)

**Checkpoint**: Full steam lifecycle + return to espresso ready

---

## Phase 7: User Story 5 - Нагадування про декальцинацію (Priority: P3)

**Goal**: Non-blocking 28×86400 s accumulated-uptime EEPROM reminder (no RTC) with USB `DC0` clear

**Independent Test**: Shorten `DECALC_INTERVAL_S` or force; one `DC1`; power-cycle mid-interval keeps accumulated seconds; USB `DC0` clears panel + EEPROM accumulator (quickstart S5)

### Implementation for User Story 5

- [X] T033 [US5] Define EEPROM layout (`DECALC_EEPROM_MAGIC`, `accumulatedOnSeconds`, `dcLatched`) and load/save helpers in `scripts/013_main_brew_logic.cpp` per `data-model.md` / research (FR-016)
- [X] T034 [US5] In loop of `scripts/013_main_brew_logic.cpp`: accumulate on-seconds (uptime only while powered), persist every `DECALC_SAVE_EVERY_S`; when `≥ DECALC_INTERVAL_S` and not latched → send one `DC1`, set `dcLatched` (FR-016, SC-008)
- [X] T035 [US5] On USB `DC0` in `scripts/013_main_brew_logic.cpp`: send panel `DC0`, clear latch, zero `accumulatedOnSeconds` and persist (FR-023); optional test `DC1` force for stand
- [X] T036 [US5] Ensure decalc path never blocks brew/heat/steam transitions in `scripts/013_main_brew_logic.cpp` (SC-008)

**Checkpoint**: All user stories independently functional on stand

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Docs, compliance, and end-to-end stand validation

- [X] T037 [P] Complete `docs/main-brew-logic.md` (FSM summary, Tunables table pointer, Serial service commands, stand wiring, differences vs protocol example for `B3`@`HS0`)
- [X] T038 [P] Update `readme.md` to list `013_main_brew_logic.cpp` and link `docs/main-brew-logic.md` / feature specs
- [ ] T039 Run magic-literal compliance check (`rg` for business literals like 120/2000/40/500/1000/3000/28 outside Tunables) on `scripts/013_main_brew_logic.cpp` per `contracts/tunable-constants.md`
- [ ] T040 Execute quickstart scenarios S1–S6 on dual-Nano stand; fix timing/SLA gaps against SC-001…SC-008
- [ ] T041 [P] Sync any Tunables default drift back into `specs/001-main-brew-logic/contracts/tunable-constants.md` if defaults changed during stand tuning

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories
- **User Stories (Phase 3–7)**: All depend on Foundational completion
  - Prefer sequential P1 → P1 → P1 → P2 → P3 on one developer (shared single file)
  - Logical independence still holds for stand validation per story
- **Polish (Phase 8)**: Depends on desired stories being complete (MVP can polish after US1 only)

### User Story Dependencies

- **User Story 1 (P1)**: After Foundational — no dependency on other stories — **MVP**
- **User Story 2 (P1)**: After Foundational; needs US1 ready state in practice (same sketch)
- **User Story 3 (P1)**: After Foundational; reuses US2 brew phase machine (T023 depends on T018 conceptually)
- **User Story 4 (P2)**: After Foundational; uses `coolOrReady()` (T011) and ready criterion from foundation/US1
- **User Story 5 (P3)**: After Foundational; orthogonal to brew/steam (can follow US1 for early stand check with shortened interval)

### Within Each User Story

- Models/helpers before state handlers
- Egress panel codes before relying on panel LEDs in stand tests
- Core happy path before edge cancels
- Story checkpoint before next priority when possible

### Parallel Opportunities

- T004 // T001–T003 (docs stub vs sketch create — limited, same author usually sequential)
- T007 // T006 after UART basics started; T010 // T009 within Foundational
- T023 can start once shared brew machine (T018) exists
- T037 / T038 / T041 docs in parallel with stand fixes after code stable
- **Note**: Primary code is one file (`013`) — true multi-dev parallel on implementation is limited; parallelize docs/compliance vs firmware owner

---

## Parallel Example: User Story 2

```bash
# After US1 checkpoint, sequential in 013 (same file) — conceptual split:
Task: "BREW_1 start P11 + counter reset"
Task: "phase machine PREINFUSE→PAUSE→FLOW"
Task: "stop B1→P10 + ignore B2/B3"
Task: "brew OLED metrics @ BREW_DISPLAY_INTERVAL_MS"
```

## Parallel Example: Polish

```bash
Task: "Complete docs/main-brew-logic.md"
Task: "Update readme.md for 013"
Task: "Sync tunable-constants.md defaults"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: quickstart S1 (+ fault smoke)
5. Demo heat/ready panel indication

### Incremental Delivery

1. Setup + Foundational → foundation ready
2. US1 → S1 → MVP demo
3. US2 → S2 → daily single shot
4. US3 → S3 → double shot
5. US4 → S4 → steam
6. US5 → S5 → decalc reminder
7. Polish → S6 + docs + literal compliance

### Parallel Team Strategy

With multiple developers (constrained by single sketch):

1. Team completes Setup + Foundational together
2. Dev A: US1→US2→US3 brew path
3. Dev B: docs (`docs/main-brew-logic.md`, readme) + stand checklist from quickstart
4. After brew MVP: Dev A US4, then US5
5. Integrate only via careful section ownership comments in `013` if splitting

---

## Notes

- [P] tasks = different files or truly independent sections; same-file [P] means no logical dependency only
- Panel firmware `scripts/011_button_panel_ui.cpp` is **out of scope** (no tasks)
- Full decalc UI on panel is **out of scope**; only main→panel `DC1`/`DC0` + USB service `DC0`
- Protocol example `B3`→`SS1` in ready-steam does **not** apply — `SS1` from handle only
- Commit after each task or logical group when implementing
- Stop at any checkpoint to validate story independently on stand
