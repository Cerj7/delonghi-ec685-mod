# Tasks: Main Brew Logic (EC685)

**Input**: Design documents from `/specs/001-main-brew-logic/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/, quickstart.md

**Tests**: Manual stand validation only (per research.md / quickstart.md). No automated unit/contract test tasks.

**Organization**: Tasks are grouped by user story. **Phase 12 = CURRENT delta** (clarify session 4: steam exit B1/B2, cold flush, LT1 pulse-cool, heat-up PI); Phase 11 = option C done (T063 stand may remain); Phases 1–10 = baseline.

**Delta scope (Phase 12)**: `scripts/013_main_brew_logic.cpp` + `docs/main-brew-logic.md` sync to session-4 plan/contracts (`BREW_1`→`COLD_FLUSH`, idle/`B3`-cancel off, FR-028, heat-up profile).

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

**Independent Test**: Power board with temp pot set; ≤1 s see `HE1`; buttons do not start pump/steam; after `Tt≥target−READY_BAND_C` for `READY_HOLD_MS` → `HE0` even if overshoot (quickstart S1)

### Implementation for User Story 1

- [X] T012 [US1] On `setup`/power-on in `scripts/013_main_brew_logic.cpp`: enter `ESPRESSO_HEAT`, set `activeTargetC` from espresso pot, send `HE1` immediately (FR-001, FR-002)
- [X] T013 [US1] In `ESPRESSO_HEAT` loop path of `scripts/013_main_brew_logic.cpp`: run PI to pot target; when ready criterion holds continuously → send `HE0`, transition `ESPRESSO_READY` (FR-002, SC-001)
- [X] T014 [US1] In button matrix for `ESPRESSO_HEAT` (and `FAULT`) in `scripts/013_main_brew_logic.cpp`: ignore `B1`/`B2`/`B3` with no pump/mode/indication side effects (FR-003)
- [X] T015 [US1] Keep pump at `PUMP_OFF_PCT` throughout heat/ready idle in `scripts/013_main_brew_logic.cpp` regardless of pump pot (FR-019)
- [X] T016 [US1] Ensure fault path in `scripts/013_main_brew_logic.cpp` latches `FAULT` (heater OFF, pump 0, OLED fault) and stays until **power cycle** — no auto-recovery (FR-018)

**Checkpoint**: US1 alone — heat → ready → buttons ignored until `HE0`

---

## Phase 4: User Story 2 - Приготування 1 еспресо з преінфузією (Priority: P1) ⚠️ SUPERSEDED by Phase 12

**Goal** *(historical)*: From ready, `B1` runs preinfusion/pause/flow… → **Session 4**: `B1` = cold flush (`COLD_FLUSH`); see Phase 12 T066–T068.

**Independent Test** *(historical S2 brew-1)* → **Current**: quickstart S2 cold flush; brew-flow only on US3/`BREW_2` (Phase 12 T069).

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
- [X] T039 Run magic-literal compliance check (`rg` for business literals like 120/2000/4000/40/500/1000/28/20.0/12.5/12.0/0.20/0.125/0.12 outside Tunables) on `scripts/013_main_brew_logic.cpp` per `contracts/tunable-constants.md` — після Phase 12 T078
- [ ] T040 Execute quickstart scenarios S1–S7 on dual-Nano stand; fix timing/SLA gaps against SC-001…SC-012 — після Phase 12 T078; physical dual-Nano stand still required — *desk-check 2026-11-12: no serial devices; SLA logic fixes in 013 (heat-up only while Tt below target; fault clears P20/SS0; EEPROM defer in STEAM_COOL); Tunables drift synced in T041; physical S1–S7 still required*
- [X] T041 [P] Sync any Tunables default drift back into `specs/001-main-brew-logic/contracts/tunable-constants.md` if defaults changed during stand tuning — після T078; T077 aligns pulse ON=110 / idle=0 first — **DONE 2026-11-12**: contract defaults = firmware Tunables (stand-tuned)

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

## Phase 11: Delta — Stand retune option C (US2 + US4) ✅ CODE DONE

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
- [ ] T063 Stand-validate S2/S4 temperature hold with option C — **deferred behind Phase 12** (re-run as part of T078 / T040 after session-4 behavior lands) — *desk-check 2026-11-12: PI select/FF/anti-windup/forced-OFF paths OK vs contracts; Tunables=stand-tuned 25/0.25+FF25 & 20/0.20+FF20 (option C evolved via T041); docs/comment drift synced; NO_SERIAL / physical dual-Nano still required. Post–Phase 12 hold scenarios: **S3** (brew-flow) + **S4** (steam-flow) (+ S7 heat-up); S2=COLD_FLUSH heater OFF*

**Checkpoint**: Option C in firmware; physical stand after Phase 12

---

## Phase 12: Delta — Clarify session 4 (US1 + US2 + US3 + US4) 🔧 CURRENT

**Purpose**: Підтягнути `013` до clarify 2026-11-12 session 4 / refreshed plan+contracts: вихід з пару лише `B1`/`B2`; `B1`=холодний пролив; `STEAM_COOL`+ручка→імпульси/ТЕН OFF; heat-up ПІ. Панель `011` **без змін**.

**Goal**:
1. Idle/`B3`-cancel off; `B1`/`B2` у `HS1`/`HS0`/`STEAM_FLOW` → `coolOrReady()`/`LT1`.
2. `BREW_1` → `COLD_FLUSH` (4 с @ 100 %, ТЕН OFF, `P11`/`P10`); `B2` лишається єдиним еспресо.
3. `STEAM_COOL` (FR-028): ТЕН OFF; відкрита ручка → `STEAM_PULSE_*`; ready → помпа 0 + `LT0` навіть якщо ручка відкрита.
4. Heat-up профіль у `ESPRESSO_HEAT`/`STEAM_HEAT`; критерії `HE0`/`HS0` без змін.

**Independent Test**: quickstart S1 (heat-up), S2 (cold flush), S3 (brew-2 only), S4 (exit via B1/B2, no idle, LT1+handle pulses, SC-011) + SC-010/012

**Depends on**: Phase 11 code; plan/contracts/data-model/quickstart already refreshed by `/speckit-plan`

### Implementation delta

- [X] T064 [P] In Tunables of `scripts/013_main_brew_logic.cpp`: set `STEAM_IDLE_EXIT_MS=0`; add `COLD_FLUSH_MS=4000`, `COLD_FLUSH_PCT=100`, `HEATER_KP_HEAT`/`HEATER_KI_HEAT` (стартово ~2.4× base per contract), `HEAT_UP_OVERSHOOT_OK_C=5.0` with FR-014/027/028 comments matching `contracts/tunable-constants.md`
- [X] T065 In `scripts/013_main_brew_logic.cpp`: rename `MachineState::BREW_1` → `COLD_FLUSH` (and all case/labels/OLED strings); update comments that still say «1 espresso preinfusion» (data-model.md)
- [X] T066 [US2] Replace brew-1 preinfusion machine with cold flush in `scripts/013_main_brew_logic.cpp`: from `ESPRESSO_READY` + `B1` → `P11`, enter `COLD_FLUSH`, reset counter, pump `COLD_FLUSH_PCT` for `COLD_FLUSH_MS`; heater forced OFF; timeout → `P10`/`ESPRESSO_READY` (FR-004, SC-002)
- [X] T067 [US2] In `COLD_FLUSH` of `scripts/013_main_brew_logic.cpp`: repeat `B1` → early stop (`PUMP_OFF_PCT`, `P10`, `ESPRESSO_READY`); ignore `B2`/`B3`; OLED brew-metrics path reuse for flush (FR-005, FR-008, SC-003/004)
- [X] T068 [US2] Block cold-flush start in `scripts/013_main_brew_logic.cpp` when already in `BREW_2` / `STEAM_HEAT` / `STEAM_FLOW` / `STEAM_COOL` (and during `HS0`/`SS0` treat `B1` as steam-exit, not flush) per FR-004/FR-022
- [X] T069 [US3] Confirm `BREW_2` path in `scripts/013_main_brew_logic.cpp` remains sole espresso preinfusion/`P21`/`P20`; brew-flow PI only for `BREW_2`+`FLOW` (remove any `BREW_1`/`COLD_FLUSH` from brew-flow selector) (FR-006/007/027, SC-010)
- [X] T070 [US4] In `onPanelButton` of `scripts/013_main_brew_logic.cpp` for `STEAM_HEAT`/`STEAM_READY`/`STEAM_FLOW`: `B1`/`B2` → `coolOrReady()`; `B3` → ignore (supersede T028/T029/T050 cancel/`B1`ignore) (FR-010/014/021/022, SC-005/007)
- [X] T071 [US4] In `STEAM_READY` of `scripts/013_main_brew_logic.cpp`: **remove/disable** idle expiry → `LT1` (T049 path); with `STEAM_IDLE_EXIT_MS=0` MUST NOT auto-cool (FR-014, SC-007)
- [X] T072 [US4] Update `coolOrReady()` in `scripts/013_main_brew_logic.cpp` per session-4 `contracts/main-fsm.md`: only entry from `B1`/`B2` steam-exit; stop steam pulses; enter `STEAM_COOL` with heater OFF; do not call from idle/`B3`/`SS0` (FR-014/028)
- [X] T073 [US4] In `STEAM_COOL` of `scripts/013_main_brew_logic.cpp`: force heater OFF; if handle open run `STEAM_PULSE_*` @ `STEAM_PULSE_PCT`; on espresso ready criterion → pump 0 even if handle open, send `LT0`, `ESPRESSO_READY`; keep `B3`→`enterSteam()`; keep `B1`/`B2` blocked for flush/brew (FR-025/028, SC-011)
- [X] T074 [US1] In `updateHeaterPi` of `scripts/013_main_brew_logic.cpp`: select heat-up (`HEATER_KP_HEAT`/`HEATER_KI_HEAT`) for `ESPRESSO_HEAT` and `STEAM_HEAT`; keep base for ready/preinfuse/pause/`STEAM_READY`; forced OFF for `COLD_FLUSH`/`STEAM_COOL`/`FAULT`; brew/steam-flow unchanged; no change to `HE0`/`HS0` criteria (FR-027, SC-010/012)
- [X] T075 [P] [US4] Sync `docs/main-brew-logic.md` to session 4: cold flush, steam exit via B1/B2, idle=0, B3 ignore in HS/SS, LT1+handle pulses, four PI profiles, panel unchanged
- [X] T076 [P] Update Phase 4/US2 wording leftovers in `specs/001-main-brew-logic/tasks.md` Notes only if needed; ensure `readme.md` still points at current docs (no panel change)
- [X] T077 Align Tunables drift from T041 notes in `scripts/013_main_brew_logic.cpp`: `STEAM_PULSE_ON_MS` → contract **110** (unless stand retunes into contract); `STEAM_IDLE_EXIT_MS` → **0** (session 4)
- [X] T078 Desk-check / stand-validate quickstart S1–S4 + S7 against SC-001…SC-012 after T064–T074; then resume Phase 8 T039–T041 / T040 / T063 — **desk-check PASS** (enterSteam open-handle→SS1 + HE0 band/hold fixed); physical dual-Nano stand still needed for T040/T063

**Checkpoint**: Session 4 behavior in `013`; physical stand → polish

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Done
- **Foundational (Phase 2)**: Done
- **User Stories (Phase 3–7)**: Done (baseline; US2 brew-1 semantics **superseded** by Phase 12)
- **Phase 9–11**: Done (code); T063 stand deferred into Phase 12/T078
- **Phase 12 Delta (session 4)**: Done (T064–T078 desk-check); physical stand → T040/T063
- **Polish (Phase 8)**: T037/T038/T039/T041 done; T040 physical stand still required

### User Story Dependencies (Phase 12)

- **US1**: T074 heat-up (after T064)
- **US2**: T065–T068 cold flush (after T064); blocks old brew-1
- **US3**: T069 brew-2-only PI selector (after T065/T066)
- **US4**: T070–T073 steam exit + cool (after T064; T072 before T073)
- **Docs/validate**: T075–T078 after code tasks

### Within Phase 12

1. T064 Tunables → T065 rename → T066–T068 cold flush // T070–T071 steam buttons/idle
2. T072 coolOrReady → T073 STEAM_COOL
3. T074 heat-up PI (can parallel with T066–T073 after T064)
4. T069 after rename/flush
5. T075–T077 docs/drift → T078 validate → Phase 8

### Parallel Opportunities (Phase 12)

- After T064: T066–T068 (US2) // T070–T071 (US4) // T074 (US1) on careful same-file sequencing
- T075 docs // late code polish T077
- T039 after Tunables settled (T064/T077)

---

## Parallel Example: Phase 12

```bash
# After T064 Tunables:
Task: "T065–T068 COLD_FLUSH in 013"
Task: "T070–T071 B1/B2 exit + idle off in 013"
Task: "T074 heat-up PI select in 013"
# Then:
Task: "T072–T073 coolOrReady + STEAM_COOL pulses"
Task: "T075 docs/main-brew-logic.md"
Task: "T078 quickstart S1–S4/S7 desk-check"
```

## Parallel Example: Polish (after Phase 12)

```bash
Task: "T039 magic-literal rg on 013 (incl. 4000 / heat gains)"
Task: "T040 physical dual-Nano S1–S7 / SC-001…SC-012"
Task: "T041 sync tunable-constants.md if stand-tuned"
```

---

## Implementation Strategy

### Current focus

1. **Phase 12** T064→T078 (session 4 behavior)
2. **Finish Phase 8**: T039 → T041; T040 physical dual-Nano stand (S1–S7)
3. T063 absorbed into T078 / T040

### MVP / prior delivery (historical)

1. Setup + Foundational → US1…US5 baseline already landed
2. Phase 9–11 = steam / PI / option C patches
3. Phase 12 = session 4 UX/FSM rewrite (cold flush + steam exit + cool + heat-up)

### Parallel Team Strategy (delta)

1. Dev A: T064–T069 cold flush / brew-2 in `013`
2. Dev B: T070–T074 steam exit / cool / heat-up in `013` (coordinate same file)
3. Together: T075–T078 docs + desk-check → T039–T041

---

## Notes

- [P] tasks = different files or truly independent sections; same-file [P] means no logical dependency only
- Panel firmware `scripts/011_button_panel_ui.cpp` is **out of scope** (no tasks) — `LT1` stays LED3 double-blink
- Full decalc UI on panel is **out of scope**; only main→panel `DC1`/`DC0` + USB service `DC0`
- Protocol example `B3`→`SS1` in ready-steam does **not** apply — `SS1` from handle only (or immediately after `HS0` if handle already open)
- `coolOrReady()` лише **`B1`/`B2` steam-exit** — **не** з `SS0`, **не** з idle, **не** з `B3`, **не** з падіння Tt у `STEAM_FLOW`
- `STEAM_IDLE_EXIT_MS=0` — idle-автовихід вимкнено (session 4); T049 path obsolete
- `B3` у `HS*`/`SS*` ignore; `B3`@`STEAM_COOL` → `enterSteam()` лишається
- **Phase 4 / US2 (T017–T022)**: historical `BREW_1` + preinfusion wording — **superseded** by Phase 12 `COLD_FLUSH` (T065–T068); Phase 4 header/checkpoint stay as baseline archive
- `P11`/`P10` = cold flush (не еспресо); sole espresso = `B2`/`P21`/`P20`
- Phase 5 «same brew profile as US2» = historical (shared preinfusion machine); current sole espresso brew = US3/`BREW_2` only
- Four PI profiles: heat-up > base; brew-flow ~5×+FF; steam-flow ~4×+FF; forced OFF in `COLD_FLUSH`/`STEAM_COOL`
- T048 obsolete (Phase 10 T056); T028/T029 cancel-via-B3 superseded by T070
- Commit after each delta task or logical group
- **Phase 11 (option C)**: стартувало brew `20/0.20`+FF20 / steam `12.5/0.125`+FF12; **T041 stand-tune** → brew `25/0.25`+FF25 (~5×) / steam `20/0.20`+FF20 (~4×) — current defaults under Phase 12
- **T041**: DONE — `tunable-constants.md` synced to firmware defaults (T077 pulse ON=110 / idle=0 kept)
- **T052/T059/T063**: historical; remaining physical validation → **T078** then **T040**
- Phase 8 **T039–T041** after Phase 12 T078
