# EXP-26 — The 4 Hz poll was only safe because nothing was listening

- **Date:** 2026-09-12
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace-meter`, branch `bench/2026-09-12`
- **Status:** **CONFIRMED** (cause, fix, and fix verified in both directions)

## 1. Problem

Minutes after EXP-25 made the meter start obeying us, the unit began clicking
its relays continuously and audibly, and kept doing it. Is that our command
cadence, and what is the correct fix — given the header must ship?

## 2. Hypothesis

`fpga_meter_poll_task` sends the stock **start word** (`05 09`) every 250 ms.
Until today every one of those was discarded on the wrong header, so the cadence
had never been exercised against a meter that obeys. Now each poll is accepted
and re-triggers a measurement and autorange.

- **If true:** the echo rate tracks the transmit rate (~4/s) with the header on,
  turning the header off stops both the echoes and the clicking, and the
  transmit rate is unchanged throughout.
- **If false:** the clicking is independent of the header state.

## 3. Procedure

```
mode meter                # MODE_MULTIMETER = 1; the poll runs only here
fpga usart on
meter hdr on|off          # EXP-25's runtime A/B
status                    # TX count, Data frames, Echo frames, last_tx_frame
```

Rates computed as deltas over 8-10 s windows rather than single reads.

## 4. Control

Transmit rate is the control variable that must **not** move: if TX changes
between phases, an echo-rate change proves nothing.

| phase | TX | DATA | ECHO |
|---|---|---|---|
| header ON | 4.1/s | 6.0/s | **~4/s** |
| header OFF | 4.1/s | 6.0/s | **0.0/s** |

TX identical to two significant figures across the flip. Clicking stopped on the
flip to OFF, confirmed by the operator in the room.

## 5. Results

### 5a. Cause confirmed

`tx_frames_recent` was a solid wall of one frame:

```
[AA 55 05 09 00 00 00 00 00 0E] × 10
```

`05 09` at 4.1/s, every one accepted and echoed. Our own poll, finally being
obeyed.

### 5b. The measurement that decided the fix

The obvious fix — slow the poll down — only makes the clicking slower. The
question is whether the poll is needed at all.

| condition | TX | DATA |
|---|---|---|
| poll running, header OFF (**zero commands accepted**) | 4.1/s | **6.0/s** |
| poll running, header ON | 4.0/s | 6.0/s |
| poll stopped (`mode scope`) | 0.00/s | **0.00/s** |

Two things follow, and the first was briefly got wrong in session:

1. **The meter does NOT free-run.** Stop transmitting and data frames stop dead.
   An intermediate reading of the 6-vs-4 rate mismatch as "it free-runs" was
   wrong and is withdrawn here rather than deleted.
2. **The meter responds to USART TRAFFIC, not to commands it accepts.** With the
   header off not one frame is obeyed — echo is flat zero — and data still
   arrives at 6/s. The rates are not even equal, so data was never
   one-frame-per-command.

**This retires a long-standing claim in its stated form.** `CLAUDE.md` has said
since 2026-04-04 that the meter "only emits data frames in response to recent TX
commands." The observation is right; the wording implies the commands were
understood. They were not, for the entire life of the project. That reading is
exactly why "the meter works" survived so long.

### 5c. Fix

The poll's job is to keep the wire busy. The command's job is to change
function. So they no longer share a header:

- An **OBEY bit** rides in the `usart_tx_queue` item (widened `uint16_t` →
  `uint32_t`), so the poll task and the shell cannot race over a shared flag.
- `fpga_send_cmd_keepalive()` sends with OBEY clear: always the `00 00` header,
  which provably still solicits data and cannot actuate anything.
- `fpga_send_cmd()` and the polled fallback set OBEY: real commands carry
  `AA 55` when the header is enabled.

### 5d. Fix verified in both directions

| test | expected | measured |
|---|---|---|
| header ON, keepalive only, 8 s | echo 0 | **0.0/s**, wire `00 00` ✅ |
| header ON, data still flowing | ~6/s | **5.9/s** ✅ |
| one explicit `usart tx 05 0C` | echo +1 | **+1** ✅ |
| 4 s of keepalive after it (16 frames) | echo +0 | **+0** ✅ |

Both halves matter. A gate that silences the keepalive by breaking commands
would pass the first test and fail the third.

### 5e. DCV read-back under a COMMANDED mode

With the gate in place and `05 0C` (DC Voltage) commanded explicitly — not the
auto mode every previous reading in this project was actually taken in:

| n | mean | min | max | spread |
|---|---|---|---|---|
| 12 | **1.6144 V** | 1.6143 | 1.6144 | 0.0001 V (0.006%) |

EXP-23's auto-mode reading of the same cell was 1.6158 V. **Delta −1.4 mV,
−0.088%.** Commanded DC Voltage and the SoC's auto mode agree to under a tenth
of a percent on this cell.

## 6. Blind spots

- **The cell is not a reference.** This shows commanded and auto agree, not that
  either is accurate. No traceable source was involved.
- **One point on the range**, ~1.6 V. Says nothing about the >10 V dp bug.
- **Relay actuation was not counted**, only heard. "Clicking stopped" is an
  operator observation, labelled as such. The echo rate going to zero is the
  measurement; the silence is corroboration.
- **The keepalive being unobeyed is measured on THIS meter.** A meter that
  reacted to `00 00` traffic differently would need this re-checked.
- Whether the meter would settle into a stable range if polled at a much lower
  obeyed rate was **not** tested — the unobeyed keepalive made it unnecessary.
- Untested: whether any *other* periodic sender in the tree becomes newly
  obeyed once the header ships. Only the meter poll was audited.

## 7. Conclusion

**Established:** the clicking was our own 4 Hz poll being obeyed for the first
time. The meter emits data in response to USART traffic rather than to accepted
commands, so the keepalive can and must be sent unobeyed. The gate holds in both
directions. Commanded DCV reads 1.6144 V, agreeing with auto mode to −0.088%.

**Withdrawn in session:** "the meter free-runs at 6 Hz" — refuted by stopping
transmit entirely.

**Reframed:** the 2026-04-04 "data frames need recent TX" claim is correct as an
observation and misleading as an explanation.

**The general lesson, which is the third instance this year:** a control with no
effect looks correct until the effect arrives. The timebase button moved a
variable nothing read; the meter word table selected functions nothing obeyed;
the meter poll ran at a cadence nothing acted on. Each looked fine precisely
because it was inert. **Anything this project drives should be assumed untested
until something on the other end has demonstrably answered.**

**Consequence for issue #15:** Stlkv's PR (1) turns the header on for everyone.
On any unit running our poll task that produces continuous relay actuation. He
cannot have seen it — his bench drives the meter by hand, not through our task.
The gate must land with, or before, the header.
