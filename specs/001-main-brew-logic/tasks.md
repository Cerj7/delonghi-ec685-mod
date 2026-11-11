# Tasks: Main Brew Logic (EC685)

**Input**: Design documents from `/specs/001-main-brew-logic/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/, quickstart.md

**Tests**: Manual stand validation only (per research.md / quickstart.md). No automated unit/contract test tasks.

**Organization**: Tasks are grouped by user story. **Phase 11 = CURRENT delta** (stand retune option C: 4×/2.5× + FF); Phase 10 = session 3 desk-check done; Phases 1–9 = baseline.

**Delta scope**: Raise brew/steam PI gains + add feed-forward % during flow in `scripts/013_main_brew_logic.cpp` + docs sync.

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

**Independent Test**: After `HE0`, press 1 espresso; verify 2 s @ 40 % → 2 s pause → pot power; OLED ≥2 Hz; second `B1` stops ≤300 ms with `P10`; `B2`/`B3` ignored (quickstart S2). **PI**: base during preinfuse/pause; brew-flow during `FLOW` (Phase 10 / SC-010).

### Implementation for User Story 2

- [X] T017 [US2] On `B1` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `P11`, enter `BREW_1`, reset water counter, start `PREINFUSE` phase (FR-004)
- [X] T018 [US2] Implement brew phase machine for `BREW_1` in `scripts/013_main_brew_logic.cpp`: `PREINFUSION_MS` @ `PREINFUSION_PCT` → `PREINFUSION_PAUSE_MS` @ `PUMP_OFF_PCT` → `FLOW` at live pump pot (ignore pot during preinfusion) (FR-004, SC-002)
- [X] T019 [US2] On repeat `B1` in `BREW_1` in `scripts/013_main_brew_logic.cpp`: pump `PUMP_OFF_PCT`, send `P10`, return `ESPRESSO_READY` within `BUTTON_REACT_MAX_MS` budget (FR-005, SC-003)
- [X] T020 [US2] Ignore `B2`/`B3` during `BREW_1` in `scripts/013_main_brew_logic.cpp` (FR-005)
- [X] T021 [US2] During `BREW_1` OLED path in `scripts/013_main_brew_logic.cpp`: every `BREW_DISPLAY_INTERVAL_MS` show water counter (pulses or ml via `FLOW_PULSES_PER_ML`), block temp, pump % (FR-008, SC-004)
- [X] T022 [US2] Keep espresso PI target from temp pot during brew in `scripts/013_main_brew_logic.cpp`; on fault abort brew (pump 0, no dangling `P11`) — *delta PI gains: Phase 10 T054/T055*

**Checkpoint**: US1+US2 — single-shot brew works end-to-end from ready; brew-flow PI gains → Phase 10

---

## Phase 5: User Story 3 - Приготування 2 еспресо з преінфузією (Priority: P1)

**Goal**: Same brew profile as US2 with `B2` / `P21` / `P20`

**Independent Test**: From ready, `B2` → same timing/power sequence → stop with `B2`/`P20`; `B1`/`B3` ignored (quickstart S3); same brew-flow PI as US2 in `FLOW`

### Implementation for User Story 3

- [X] T023 [P] [US3] On `B2` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `P21`, enter `BREW_2`, reset counter, reuse shared brew phase machine (FR-006)
- [X] T024 [US3] On repeat `B2` in `BREW_2` in `scripts/013_main_brew_logic.cpp`: pump off, send `P20`, return `ESPRESSO_READY`; ignore `B1`/`B3` during `BREW_2` (FR-007)
- [X] T025 [US3] Confirm `BREW_2` uses same OLED brew display path as `BREW_1` in `scripts/013_main_brew_logic.cpp` (FR-008)

**Checkpoint**: Both shot modes independently stoppable; foreign buttons ignored

---

## Phase 6: User Story 4 - Режим пару (Priority: P2)

**Goal**: Steam heat to 120 °C, handle-driven pulses, idle/`B3` cancel via cool path; ignore espresso buttons in steam/cool

**Independent Test**: From ready run quickstart S4 (оновлений — Phase 9 + **Phase 10**)

### Implementation for User Story 4

> **Superseded by Phase 9 + Phase 10**. T026–T032 = baseline; T042–T051 = session 2; T048 behavior superseded by Phase 10 T056 (no flow pause).

- [X] T026 [US4] On `B3` from `ESPRESSO_READY` in `scripts/013_main_brew_logic.cpp`: send `HS1`, enter `STEAM_HEAT`, `activeTargetC=STEAM_TARGET_C` (FR-009, FR-013) — *delta: T044 `enterSteam()`*
- [X] T027 [US4] In `STEAM_HEAT` of `scripts/013_main_brew_logic.cpp`: heat to steam target; on `|Tt−STEAM_TARGET_C|≤STEAM_READY_BAND_C` send `HS0` immediately (**no** `READY_HOLD_MS`) and enter `STEAM_READY` (FR-011, SC-005) — *delta: T043/T045 `Tt ≥ STEAM_TARGET_C`*
- [X] T028 [US4] On `B3` during `STEAM_HEAT` in `scripts/013_main_brew_logic.cpp`: cancel via `coolOrReady()` (FR-010)
- [X] T029 [US4] In `STEAM_READY` of `scripts/013_main_brew_logic.cpp`: open handle → `SS1`/`STEAM_FLOW`; closed idle `STEAM_IDLE_EXIT_MS` → `LT1`/`STEAM_COOL`; `B3` → `coolOrReady()` without `SS1` (FR-012, FR-014, FR-021, SC-006/007) — *delta: T042 idle=3000, T046 auto-reheat*
- [X] T030 [US4] Implement steam pulse pump in `STEAM_FLOW` of `scripts/013_main_brew_logic.cpp`: `STEAM_PULSE_ON_MS`/`STEAM_PULSE_OFF_MS` @ `STEAM_PULSE_PCT`; handle close → `SS0`, pump 0, then `coolOrReady()` (not idle `HS0` / 3 s) (FR-012, FR-015, SC-006) — *delta: T047 close→HS0*
- [X] T031 [US4] Complete `STEAM_COOL` in `scripts/013_main_brew_logic.cpp`: target espresso pot; ready criterion → `LT0` → `ESPRESSO_READY`; ignore `B1`/`B2` in all steam/cool states; ignore `B3` in `STEAM_FLOW`/`STEAM_COOL` (FR-014, FR-022) — *delta: T050 `B3`→enterSteam*
- [X] T032 [US4] Hold steam PI near `STEAM_TARGET_C` until exit and keep `SAFETY_CUTOFF_C` protective in `scripts/013_main_brew_logic.cpp` (FR-013, FR-018) — *delta: T046/T049 maintain target; Phase 10 steam-flow gains*

**Checkpoint**: Baseline steam landed; complete **Phase 10** before stand S4 pass against current spec

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
- [ ] T039 Run magic-literal compliance check (`rg` for business literals like 120/2000/40/500/1000/3000/28/20.0/12.5/0.20/0.125 outside Tunables) on `scripts/013_main_brew_logic.cpp` per `contracts/tunable-constants.md` — після Phase 11 T063
- [ ] T040 Execute quickstart scenarios S1–S6 on dual-Nano stand; fix timing/SLA gaps against SC-001…SC-010 — **UNBLOCKED**; physical dual-Nano stand still required (T059 = desk-check only)
- [ ] T041 [P] Sync any Tunables default drift back into `specs/001-main-brew-logic/contracts/tunable-constants.md` if defaults changed during stand tuning — **UNBLOCKED**; desk-check T059 found `STEAM_PULSE_ON_MS=300` (contract/SC-006: 500) and `STEAM_IDLE_EXIT_MS=5000` (contract/T042: 3000) in `013` Tunables

---

## Phase 9: Delta — Steam clarify session 2 (US4) ✅ DONE

**Purpose**: Підтягнути `scripts/013_main_brew_logic.cpp` до clarify session 2. Не переписувати brew/decalc.

**Goal**: Пар: `Tt ≥ STEAM_TARGET_C` → `HS0`; idle 3 с → `LT1`; `SS0` → `HS0`; авто-догрів у `STEAM_READY`; `B3`@`STEAM_COOL` → `enterSteam()`

**Independent Test**: quickstart S4 session-2 subset (overshoot, close→HS0, idle→LT1, B3@cool). **Note**: flow Tt-drop behavior replaced by Phase 10.

**Depends on**: Phase 6 baseline; contracts session 2

### Implementation delta

- [X] T042 [US4] In Tunables of `scripts/013_main_brew_logic.cpp`: set `STEAM_IDLE_EXIT_MS=3000`, set `STEAM_READY_BAND_C=0.0f` (unused for HS0) with comments pointing to FR-011/FR-014 and `contracts/tunable-constants.md`
- [X] T043 [US4] Change steam-ready helper in `scripts/013_main_brew_logic.cpp` from `|Tt−STEAM_TARGET_C|≤STEAM_READY_BAND_C` to **`Tt ≥ STEAM_TARGET_C`** (no hold, no upper band; FR-011, SC-005)
- [X] T044 [US4] Add `enterSteam()` in `scripts/013_main_brew_logic.cpp` per `contracts/main-fsm.md`: `activeTargetC=STEAM_TARGET_C`; if `Tt ≥ STEAM_TARGET_C` → send `HS0`/`STEAM_READY` (+ reset idle clock); else `HS1`/`STEAM_HEAT`. Wire `B3` from `ESPRESSO_READY` through `enterSteam()` (FR-009)
- [X] T045 [US4] In `STEAM_HEAT` path of `scripts/013_main_brew_logic.cpp`: on steam-ready → `HS0`/`STEAM_READY`; if steam handle already open → immediately `SS1`/`STEAM_FLOW` (FR-026 resume from heat)
- [X] T046 [US4] In `STEAM_READY` of `scripts/013_main_brew_logic.cpp`: if `Tt < STEAM_TARGET_C` → send `HS1`, enter `STEAM_HEAT` (**not** `coolOrReady`); keep PI on `STEAM_TARGET_C` while handle open or closed until idle/cancel (FR-026, FR-013)
- [X] T047 [US4] In `STEAM_FLOW` of `scripts/013_main_brew_logic.cpp`: handle close → `SS0`, pump `PUMP_OFF_PCT`, return **`STEAM_READY`** and start/continue idle timer — **MUST NOT** call `coolOrReady()` (FR-015, SC-006)
- [X] T048 [US4] ~~In `STEAM_FLOW`: if `Tt < STEAM_TARGET_C` → `SS0`, pump 0, enter `STEAM_HEAT`~~ — **SUPERSEDED by Phase 10 T056** (session 3: stay in flow, no HS1/pause)
- [X] T049 [US4] In `STEAM_READY` idle of `scripts/013_main_brew_logic.cpp`: count `STEAM_IDLE_EXIT_MS` only with handle closed and no button events; open handle / `STEAM_FLOW` resets or pauses timer; on expiry → `LT1`/`STEAM_COOL` + drop steam target via same cool path as cancel (FR-014, SC-007)
- [X] T050 [US4] In `onPanelButton` for `STEAM_COOL` in `scripts/013_main_brew_logic.cpp`: `B3` → `enterSteam()` (FR-025, SC-009); keep `B1`/`B2` ignored; keep `B3` ignored only in `STEAM_FLOW` (FR-022)
- [X] T051 [P] [US4] Verify `docs/main-brew-logic.md` FSM/rules match Phase 9 behavior; fix any leftover «close→coolOrReady» / «idle=0» / «± band» wording
- [X] T052 [US4] Stand-validate quickstart S4 against SC-005…SC-010 (**після Phase 10**; includes continuous pulse + PI profiles) — **closed via T059 desk-check** (S4 continuous pulse + steam-flow PI / SC-006·SC-010 code paths); full physical S4 + SC-005…SC-009 timings remain under T040

**Checkpoint**: Session 2 steam landed; **Phase 10 required** before claiming S4 pass on current spec

---

## Phase 10: Delta — Clarify session 3 (US2 + US4) ✅ DONE (desk-check)

**Purpose**: Continuous steam pulse below 120 °C + three PI profiles (base / brew-flow 3× / steam-flow 2×) per clarify 2026-11-11 session 3, `plan.md`, `contracts/main-fsm.md`, `tunable-constants.md`.

**Goal**:
1. `STEAM_FLOW` + `Tt < STEAM_TARGET_C` → stay in flow, keep pulses, **no** `HS1`/`SS0` (FR-026).
2. Auto-`HS1` **only** from `STEAM_READY` with handle closed.
3. PI: base idle; brew-flow only in brew `FLOW`; steam-flow entire `STEAM_FLOW` (FR-027 / SC-010).

**Independent Test**: quickstart S2 (PI switches at `FLOW`) + S4 steps 8–9 (closed-handle auto-HS1; open-handle continuous pulse below target) + SC-010

**Depends on**: Phase 9 code; contracts/plan already refreshed by `/speckit-plan`

### Implementation delta

- [X] T053 [P] Add Tunables `HEATER_KP_BREW=15.0`, `HEATER_KI_BREW=0.15`, `HEATER_KP_STEAM=10.0`, `HEATER_KI_STEAM=0.10` (comments FR-027 / 3× & 2× of base) in `scripts/013_main_brew_logic.cpp` matching `specs/001-main-brew-logic/contracts/tunable-constants.md`
- [X] T054 In heater PI step of `scripts/013_main_brew_logic.cpp`: select active Kp/Ki — brew-flow iff `(BREW_1|BREW_2) && BrewPhase==FLOW`; steam-flow iff `STEAM_FLOW`; else base (`HEATER_KP`/`HEATER_KI`); switch within ≤ one `CONTROL_INTERVAL_MS`; keep `SAFETY_CUTOFF_C` common; no target-offset / no feed-forward (FR-027, SC-010)
- [X] T055 [US2] Confirm brew path in `scripts/013_main_brew_logic.cpp`: PREINFUSE/PAUSE use base PI; entering `FLOW` uses brew-flow gains; leaving brew (`P10`/`P20` / fault) returns to base (FR-027, SC-010; quickstart S2)
- [X] T056 [US4] In `STEAM_FLOW` of `scripts/013_main_brew_logic.cpp`: **remove** T048 branch (`Tt < STEAM_TARGET_C` → `SS0`/pump 0/`STEAM_HEAT`); keep pulse pump running while handle open; continue heating with steam-flow PI; **MUST NOT** send `HS1`/`SS0` solely due to temp drop (FR-026, SC-006)
- [X] T057 [US4] In `STEAM_READY` of `scripts/013_main_brew_logic.cpp`: keep auto-`HS1` only when handle is **closed** and `Tt < STEAM_TARGET_C`; do not apply that transition while already in `STEAM_FLOW` (FR-026)
- [X] T058 [P] [US4] Update `docs/main-brew-logic.md` if any leftover «flow Tt drop → HS1 / pause pump» wording remains; confirm three PI profiles documented
- [X] T059 [US4] Stand-validate quickstart S2 (brew-flow) + S4 (continuous pulse below 120 + steam-flow PI) against SC-006/SC-010; then resume Phase 8 T039–T041 / close T052 — **desk-check; hardware stand pending** (see Notes). Phase 8 T039–T041 unblocked; T052 closed.

**Checkpoint**: Phase 10 code + desk-check done; **Phase 11** raises gains + FF after stand showed B too weak

---

## Phase 11: Delta — Stand retune option C (US2 + US4) 🔧 CURRENT

**Purpose**: Після стенду 3×/2× без FF виявилось мало → option **C**: brew **4×** + FF, steam **2.5×** + FF (FR-027 / SC-010).

**Goal**:
1. Tunables: `HEATER_KP/KI_BREW=20/0.20`, `HEATER_FF_BREW_PCT=20`; `HEATER_KP/KI_STEAM=12.5/0.125`, `HEATER_FF_STEAM_PCT=12`.
2. `updateHeaterPi`: додавати FF лише в brew-`FLOW` / `STEAM_FLOW`, clamp 0…100 з anti-windup.
3. Docs/contracts already refreshed; verify `013` + `docs/main-brew-logic.md`.

**Independent Test**: quickstart S2/S4 — менше просідання Tt під час flow vs попередні 3×/2×; SC-010

### Implementation delta

- [X] T060 [P] Update Tunables in `scripts/013_main_brew_logic.cpp` to option C gains + `HEATER_FF_BREW_PCT` / `HEATER_FF_STEAM_PCT` per `contracts/tunable-constants.md`
- [X] T061 In `updateHeaterPi` of `scripts/013_main_brew_logic.cpp`: apply selected FF % additively during brew-`FLOW` / `STEAM_FLOW` only; include FF in anti-windup raw; clamp output 0…100 (FR-027)
- [X] T062 [P] Sync `docs/main-brew-logic.md` PI section to 4×/2.5× + FF (if not already)
- [ ] T063 Stand-validate S2/S4 temperature hold with option C; then T039–T041 / T040

**Checkpoint**: Option C in firmware; physical stand confirms less droop → polish

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Done
- **Foundational (Phase 2)**: Done
- **User Stories (Phase 3–7)**: Done (baseline)
- **Phase 9 Delta (US4 session 2)**: Code done; T052 closed via T059 desk-check
- **Phase 10 Delta (US2+US4 session 3)**: **DONE** (T053–T059); superseded gains by Phase 11
- **Phase 11 Delta (option C)**: **CURRENT** — T060–T062 code/docs done; T063 stand
- **Polish (Phase 8)**: T037/T038 done; **T039–T041 after T063**

### User Story Dependencies

- **User Story 1, 3, 5**: Complete (no open delta)
- **User Story 2 (P1)**: Baseline + Phase 10 + **Phase 11 brew 4×+FF**
- **User Story 4 (P2)**: Baseline + Phase 9/10 + **Phase 11 steam 2.5×+FF**

### Within Phase 11

1. T060 Tunables ✅ → T061 FF in PI ✅ → T062 docs ✅ → T063 stand

### Parallel Opportunities (delta)

- T062 docs // T060–T061 code (already synced)
- T039 after T060/T061 constants settled

---

## Parallel Example: Phase 11

```bash
Task: "T060–T061 option C gains + FF in 013"
Task: "T062 docs PI section"
# Then stand:
Task: "T063 S2/S4 temp hold"
```

## Parallel Example: Polish (after Phase 11)

```bash
Task: "T039 magic-literal rg on 013"
Task: "T040/T063 quickstart S1–S6 incl. SC-010"
Task: "T041 sync tunable-constants.md if tuned"
```

---

## Implementation Strategy

### Current focus

1. ~~**Phase 10** T053→T059~~ ✅
2. **Phase 11** T060–T062 ✅ code; **T063** stand retune
3. **Finish Phase 8**: T039 → T041; T040 physical dual-Nano stand (S1–S6)

### MVP / prior delivery (historical)

1. Setup + Foundational → US1…US5 baseline already landed
2. Phase 9 = session 2 steam patch
3. Phase 10 = session 3 behavior patch (US2 PI + US4 flow)

### Parallel Team Strategy (delta)

1. ~~Dev A: T053–T057 in `013`~~ ✅
2. ~~Dev B: T058 docs wording check~~ ✅
3. ~~Together: T059 desk-check~~ ✅ → Phase 11 option C → **T063 stand**, then T039–T041

---

## Notes

- [P] tasks = different files or truly independent sections; same-file [P] means no logical dependency only
- Panel firmware `scripts/011_button_panel_ui.cpp` is **out of scope** (no tasks)
- Full decalc UI on panel is **out of scope**; only main→panel `DC1`/`DC0` + USB service `DC0`
- Protocol example `B3`→`SS1` in ready-steam does **not** apply — `SS1` from handle only (or immediately after `HS0` if handle already open)
- `coolOrReady()` лише cancel/`idle` — **не** з `SS0` і **не** з падіння Tt у `STEAM_FLOW`
- T048 (flow pause on cold) is **obsolete** — implement T056 instead
- Brew-flow > steam-flow > base; option **C**: 4×/2.5× + FF during flow (B 3×/2× без FF — мало на стенді)
- Commit after each delta task or logical group
- **T059 desk-check (2026-11-11)**: session-3 FSM/selector code-complete; gains later raised in Phase 11.
- **Phase 11 (option C)**: brew `20/0.20` + FF 20 %; steam `12.5/0.125` + FF 12 %; FF only in brew-`FLOW` / `STEAM_FLOW`.
  - **SC-006 / S4 step 9 (FR-026)**: `STEAM_FLOW` has no Tt-drop → `HS1`/`SS0`/pause branch (T056); pulses continue via `STEAM_PULSE_*` while handle open; heater stays on `TARGET_STEAM`. Auto-`HS1` only in `STEAM_READY` with handle closed (T057).
  - **SC-006 pulse/OLED/close**: handle open → `SS1` + pulse ON/OFF @ `STEAM_PULSE_PCT`; OLED `OPEN`; close → pump 0 + `SS0` → `STEAM_READY` (not `coolOrReady`).
  - **T041 follow-ups (defaults drift in Tunables, not Phase 10 logic)**: `STEAM_PULSE_ON_MS=300` vs contract/SC-006 **500**; `STEAM_IDLE_EXIT_MS=5000` vs contract/T042 **3000**.
- **T052**: closed by T059 desk-check of Phase 10 S4 subset; remaining physical S1–S6 / SC-001…SC-010 → **T040**
- Phase 8 **T039–T041 resumed** after T059
