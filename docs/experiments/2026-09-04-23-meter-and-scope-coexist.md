# EXP-23 — Meter and scope coexist today; only submode selection is dead

- **Date:** 2026-09-04
- **Unit:** bench unit #1
- **Build:** `guest-coldtrace` @ `Sep  4 2026 06:28:04` (HEAD of `main`, `44ed093`)
- **Status:** **CONFIRMED**

## 1. Problem

Milestone M1 ("one image that is the whole instrument") was scoped on the
assumption, carried by `README.md` and `docs/specs/meter/meter-in-the-scope-build.md`,
that the multimeter is **dead** in the shippable scope build. EXP-05 (2026-08-17)
recorded the opposite three days *before* that spec was written. Which is true on
today's HEAD, and if the meter does run, what part of it doesn't?

## 2. Hypothesis

`FPGA_WARM_HANDOFF_TEST` does double duty: it means both "the FPGA is already
configured, skip config" and "this is a scope-only image, no-op every meter
entry point". Static reading of `fpga.c` says `fpga_set_meter_mode()`,
`fpga_meter_reinit()`, `fpga_scope_wake()` and `fpga_enter_scope_mode()` are all
hard `return`s under it, while `fpga_set_meter_mux()` (called from `main.c`'s
mode-transition block) is **not**.

- **If true:** the meter reads on DCV — the mux path is intact — but selecting
  any other submode changes nothing, because the only code that re-postures the
  relays and sends the submode frames is the no-opped one.
- **If false (meter genuinely dead):** no valid frame ever decodes, in any
  submode, with a known source on the leads.

## 3. Procedure

Flashed `firmware/build/firmware.bin` via the factory IAP channel
(`scripts/iap_flash.py`). CDC re-enumerated in 3 s. All observation over
`/dev/ttyACM0` via `scripts/bench.py`.

Preconditions verified by readback, not assumed:

| precondition | expected | read back |
|---|---|---|
| config path actually taken | bit-bang | `path: GPIO bit-bang (FPGA_CONFIG_B)` |
| bitstream actually sent | 115638 | `115638 / 115638`, `Upload done: YES` |
| scope actually capturing | counter climbs | `SPI3 OK` 451 → 566 → still climbing at t+10 min |
| USART2 actually up in meter mode | UEN=1 | `CTRL1=0000202C BAUDR=000030D4 UEN=1 TE=1 RE=1 RDBFIEN=1` |
| PC11 actually high | 1 | `gpio scan` → `PC11 (meter MUX): 1` |
| boot mode | forced scope | `mode` → `current=0` (`MODE_OSCILLOSCOPE`) despite `startup=Meter` |

Then: `mode meter 0`, 1.5 V AA cell held across the leads for ~45 s while
polling `meter dump` at ~3 Hz and logging every distinct frame.

Submode test: `gpio scan` + `status` captured under `mode meter 0` (DC Voltage)
and `mode meter 6` (Resistance), diffed.

## 4. Control

| control | expected | measured | passed? |
|---|---|---|---|
| **positive — meter path can produce a reading at all** | reads the cell | **1.6158 V**, `valid=1`, `reject=0` | ✅ |
| independent reference (user's stock reading, same cell) | agrees | 1.61 V | ✅ |
| **positive — `gpio scan` can detect a relay change** | PE4/5/6 move | `fpga scope range 5`→`9` moved **PE5 1→0→1, PE6 1→1→0** | ✅ |
| scope still alive during the meter run | counter climbs | `SPI3 OK` climbing throughout | ✅ |

The second control is the one that matters. Without it, "GPIO posture identical
across submodes" would be exactly the Exp-F failure mode — an instrument that
could not have seen the thing it was used to exclude.

## 5. Results

**The meter reads.** Distinct frames captured over 45 s with the cell attached:

```
t= 0.1  5A A5 E4 2E 63 25 07 …   f6=07  digits 0A 0B 0C 0D  reject=1  disp=---      (open leads)
t= 3.3  5A A5 EE 07 CA E7 …      f6=0F  digits 06 01 05 08  reject=0  disp=1.6158 V
t= 5.4  5A A5 EE 07 CA C7 …      f6=0F  digits 06 01 05 09  reject=0  disp=1.6159 V
t=20.9  5A A5 EE 07 CA 87 …      f6=0A  digits 06 01 05 07  reject=0  disp=1.6157 V
t=45.8  5A A5 EE 07 0A CA …      f6=0F  digits 06 01 01 09  reject=0  disp=1.6119 V
```

`raw=16158 dp=1 unit=V class=1`. `rx_bytes` 0 → 7488+ and climbing; `data_frames`
tracking it; `echo_frames` **0** (same open detail EXP-05 recorded).

**The open-leads idle frame is a fixed pattern**, not a fault: bytes 2–5 pinned
at `E4 2E 63 25` across 800+ updates, only byte[11] toggling `18`/`19`. It
decodes to BCD digits `0A 0B 0C 0D` — out of range — so the decoder rejects it
and the display honestly shows `---`. This frame was mistaken for a dead link
earlier in the same session, before the cell was attached.

**Submode selection is completely inert.** DC Voltage vs Resistance:

- `gpio scan` diff: **empty**. PC12/PE4/PE5/PE6/PA15/PA10/PB10 bit-identical.
- `last_tx_frame` identical: `00 00 05 09 00 00 00 00 00 0E` in both.
- All 11 submodes accept the command and update the UI label; none reach the FPGA.

Manually re-sending stock's activation words (`0x0508 0x0507 0x050A 0x0514
0x0509`) does not change the open-leads payload either.

**Instrument defect found (recorded because it silently corrupts bench notes).**
`fpga cmd <hi> <lo>` and `fpga frame <b…>` parse their arguments with
`parse_int()`, which is **decimal** unless `0x`-prefixed, while the neighbouring
`usart raw` uses `parse_hex32()`. `fpga cmd 05 14` reports `TX [05 0E]` — it
transmits `0x050E` and prints a plausible confirmation of the wrong command. It
does not error. Both commands' own help text shows hex-looking examples, and
`fpga frame`'s documented example (`fpga frame 00 0B 01 …`) would in fact fail on
`0B`. Any bench note using the two-argument form with a digit pair ≥ 10 sent
something other than what it recorded.

## 6. Blind spots

- Only **DCV** was exercised with a real source. The claim about other submodes
  is that they are *unreachable*, not that they would misread if reached.
- The submode test compares **GPIO levels and the TX frame**. A submode change
  that acted purely through SPI3, or through a pin outside `gpio scan`'s list,
  would not appear — though `fpga_set_meter_mode()` returning before its first
  statement makes that moot for this build.
- One cell, one voltage, one unit. Nothing here speaks to meter accuracy across
  range, and 1.6158 vs a *user-read* 1.61 agrees only to the precision of the
  reference.
- The scope was confirmed alive by a **counter**, not by looking at a trace.
  `SPI3 OK` climbing proves reads are completing, not that the samples are good.

## 7. Conclusion

- **Established:** meter and scope run **simultaneously** in a single
  `guest-coldtrace` image on today's HEAD. DCV reads 1.6158 V on a 1.61 V cell
  while SPI3 acquisition continues. M1's premise — that coexistence needs
  bring-up — is **false**; coexistence has been working since EXP-05 on
  2026-08-17.
- **Established:** the real gap is **submode selection**, inert because
  `FPGA_WARM_HANDOFF_TEST` no-ops `fpga_set_meter_mode()`. The build is stuck in
  whatever posture it booted with, which happens to be DCV-compatible. Boot mode
  is likewise force-pinned to scope.
- **Two shipped documents are wrong** and are corrected in this commit:
  `README.md` ("It holds USART2 dark … so the meter is inactive in this build")
  and `docs/specs/meter/meter-in-the-scope-build.md` ("dead in
  `guest-coldtrace`", plus two hypotheses about what the FPGA config "kills").
  The spec was written 2026-08-20, three days *after* EXP-05 measured the
  opposite, and has been steering work since.
- **NOT established:** that any non-DCV submode works once reachable. Making
  `fpga_set_meter_mode()` run in this build is a prerequisite for finding out,
  not a fix in itself.
- **NOT established:** why `echo_frames` stays 0. Unchanged open detail from
  EXP-05.
