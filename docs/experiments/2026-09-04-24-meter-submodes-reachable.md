# EXP-24 — Meter submodes made reachable in a cold-configured build

- **Date:** 2026-09-04
- **Unit:** bench unit #1
- **Build:** `guest-coldtrace-meter` @ `Sep  4 2026 08:55:01` (new target, this commit)
- **Status:** **CONFIRMED for the frontend/transport claim; DCV read-back and the
  non-DCV submodes are NOT yet measured** (see §6, §7)

## 1. Problem

EXP-23 established that the meter and scope already coexist in `guest-coldtrace`,
and that the real gap is submode selection: all 11 submodes drive a bit-identical
analog frontend and an identical TX frame, because `FPGA_WARM_HANDOFF_TEST`
no-ops `fpga_set_meter_mode()`. Can that be lifted without costing the scope?

The risk is specific and not hypothetical: **the analog frontend is shared
between meter and scope.** Letting meter entry posture it per submode means a
trip to the meter can leave the scope wrong.

## 2. Hypothesis

`FPGA_WARM_HANDOFF_TEST` fuses two independent meanings — "the FPGA is already
configured, skip config" and "this is a scope-only image, no-op the meter". A
cold-configured build (`FPGA_CONFIG_B`) needs the first and has no reason to
inherit the second: it configured the part itself, so there is no stock-armed
state to protect.

Splitting them out as `FPGA_METER_SUBMODES`, and restoring the scope's relay
posture on scope entry, should make submodes reachable at no cost to capture.

- **If true:** each submode produces a *distinct* `gpio scan` posture, the
  mode-entry command frames appear on the wire, and `SPI3 OK` still climbs after
  a meter visit with the scope frontend restored bit-identically.
- **If false:** either the postures stay identical (the no-op was not the only
  blocker), or the scope frontend does not come back and capture degrades.

## 3. Procedure

Change, in three parts:

1. New `FPGA_METER_SUBMODES` macro (default **0**, so every existing build is
   unaffected). `fpga_set_meter_mode()` / `fpga_meter_reinit()` no-ops become
   `#if FPGA_WARM_HANDOFF_TEST && !FPGA_METER_SUBMODES`.
2. `fpga_set_meter_mux(false)` — the per-mode hook `main.c` already calls on
   every transition — restores `PB11` HIGH and `fpga_set_scope_frontend_ranges()`
   on scope entry. **Relays only**: deliberately not `fpga_scope_reinit()`, which
   would also re-send USART sequences and reset `data_ready`. This build's
   configured-and-armed FPGA state is not something to spend on a mode switch.
3. New target `guest-coldtrace-meter` = `guest-coldtrace` + `-DFPGA_METER_SUBMODES=1`.

Flashed via the factory IAP channel; CDC re-enumerated on its own. All
observation over `/dev/ttyACM0` via `scripts/bench.py`.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| **positive — `gpio scan` can see relay changes** | posture moves | inherited from EXP-23 same rig: `fpga scope range 5`→`9` moved PE5, PE6 | ✅ |
| **negative control — the same test on the previous build** | all diffs empty | EXP-23: DCV vs Resistance diff **empty** | ✅ |
| scope alive before the meter visit | counter climbs | `SPI3 OK` 875 → 923 | ✅ |
| host suites still green | all pass | 96 tests / 13 suites + 7 firmware unit targets | ✅ |

The negative control is what gives this result meaning: the identical procedure
on the pre-change build produced empty diffs, so a non-empty diff here is the
change and not the instrument.

## 5. Results

**Submodes now posture the frontend, and each one differently:**

| transition | `gpio scan` diff |
|---|---|
| DCV → Resistance | PE5 0→1, PE6 1→0, PA10 1→0, PB10 0→1 |
| DCV → Continuity | PC12 1→0, PA15 1→0, PB10 0→1 |
| DCV → Diode | PB11 1→0, PC12 1→0, PE4 1→0, PA15 1→0, PB10 0→1 |

All three were **empty** on the previous build.

**The mode-entry command frames reach the wire.** `TX count` 117 → 121 across a
single DCV → Resistance switch, and `tx_frames_recent` shows stock's sequence:

```
[00 00 05 08 …]  configure
[00 00 05 14 …]  variant/setup
[00 00 05 07 …]  probe
[00 00 05 09 …]  start/poll (repeats at the 250 ms cadence)
```

So both halves of stock's mode-entry sequence — pins **and** commands — now
happen. `last_tx_frame` alone still reads `05 09` because the poll overwrites it;
that field is not a usable detector for a transition.

**The scope survives a meter visit.** Diode was chosen deliberately as the most
disruptive submode (it moves PB11, PC12, PE4, PA15, PB10):

```
scope BEFORE meter visit: SPI3 OK  875 ->  923   climbing
   (visit Diode)
scope AFTER  meter visit: SPI3 OK 1008 -> 1057   climbing
frontend restored: diff EMPTY
```

**Accidental observation, weak evidence, recorded because it is the first of its
kind:** a leftover Diode frame in the dump buffer decoded as
`sub=8 cls=5 disp=OL family=4/4 reject=0` — a *valid* decode in a non-DCV
submode, with "overload" being the correct reading for open leads on diode. One
frame, unplanned, no source attached. Suggestive only.

## 6. Blind spots

- **The scope was confirmed by a counter, not by a trace.** `SPI3 OK` climbing
  proves reads complete; it does not prove the samples are good. A frontend
  restored to the right *pin levels* could still have left the analog path
  disturbed in a way only a waveform would show.
- **`gpio scan` reads levels on a fixed pin list.** A submode difference acting
  through SPI3, or through a pin outside that list, is invisible here.
- **No source was on the leads for any part of this experiment.** Everything
  above is about transport and posture, not measurement.
- The restore was tested for **one** cycle (scope → Diode → scope). Repeated
  cycling, and meter → scope → meter, are untested.
- Only bench unit #1.

## 7. Conclusion

- **Established:** `FPGA_METER_SUBMODES` makes all meter submodes reachable in a
  cold-configured build. Each drives a distinct analog-frontend posture and
  sends stock's mode-entry command sequence, where the previous build sent
  nothing and moved nothing.
- **Established:** a meter visit does not cost the scope. Capture continues and
  the scope frontend is restored bit-identically after the most disruptive
  submode, one cycle.
- **NOT established — and this gates promoting the flag into `guest-coldtrace`:**
  1. that DCV still *reads* correctly now that the transition actually runs.
     EXP-23 measured 1.6158 V on the old build; the equivalent measurement has
     not been taken on this one. Two capture attempts elapsed with no source
     attached.
  2. that any non-DCV submode **works** rather than merely being reachable.
     Nothing on record has ever tested one. The `OL` frame above is a hint, not
     a measurement.
- **Recommendation:** leave `guest-coldtrace` unchanged and `FPGA_METER_SUBMODES`
  opt-in until both are measured. The acceptance checklist is in the Makefile
  target's comment so it does not drift from the flag it guards.
