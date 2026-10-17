# EXP-25 — The meter TX header: `AA 55` replicates on unit #1

- **Date:** 2026-09-12
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace-meter`, branch `bench/2026-09-12`
- **Status:** **CONFIRMED** — and it took two attempts, the first of which was VOID

## 1. Problem

`echo_frames` has been 0 on every build since EXP-05 (2026-08-17). Without an
acknowledgement channel, a meter command that is silently ignored is
indistinguishable from one that worked — which is precisely the state EXP-24
left us in, having proved the submodes *reachable* without being able to show
any of them *accepted*.

On 2026-09-07 Stlkv measured on **unit #2** (issue #15) that the meter SoC
requires a `AA 55` frame header; ours sends `00 00`. Does that replicate here?

## 2. Hypothesis

The meter SoC ignores frames that do not begin `AA 55`.

- **If true:** flipping the header to `AA 55` makes `echo_frames` move off zero,
  and flipping back to `00 00` stops it. Data frames continue in both states.
- **If false:** `echo_frames` stays 0 with `AA 55`, with `echo_start` also 0,
  which would put the difference between the two units on the wire and make
  this a genuine cross-unit divergence rather than a dud.

## 3. Procedure

Header made **runtime-toggleable** (`meter hdr on|off`) rather than
compile-time, so the comparison is A/B/A inside one boot: same probe, same
build, same cell, one variable. Defaults **off**, so the device boots
bit-identical to the one that took every earlier measurement.

```
fpga usart on            # coldtrace leaves USART2 dark; nothing can TX until this
fpga meter reinit 0      # full meter entry (frontend posture + boot sequence)
meter hdr [on|off]
usart tx 05 <word>       # words from Stlkv's measured 12-word map
status                   # last_tx_frame = the bytes ACTUALLY sent
```

**Preconditions verified by readback** (not assumed):

| what | expected | measured |
|---|---|---|
| USART2 `CTRL1` after `fpga usart on` | UEN/TE/RE/RDBFIEN set | `0x0000202C` ✅ |
| USART2 `BAUDR` | 9600 off 120 MHz PCLK1 | `0x000030D4` ✅ |
| scope still running | SPI3 OK climbing | 245 and climbing ✅ |
| **the header actually on the wire** | `AA 55 ...` | see §5 — **this is the one that mattered** |

## 4. Control

**Positive control: data frames.** The meter must be demonstrably talking to us
through the same ISR and the same frame-sync machine before a zero echo count
means anything.

- After `fpga usart on` alone: `tx_count=2`, `rx_bytes=0`, `data_frames=0`.
  **Not yet a control** — frames went out, nothing came back.
- After `fpga meter reinit 0`: `rx_bytes=133`, `data_frames=11`, climbing
  steadily thereafter (94, 316, …). **Control established.** The RX path works
  on this unit in this session.
- Throughout every phase below, data frames kept climbing. The control holds in
  all five phases, not just at the start.

## 5. Results

### 5a. First run — **VOID**, and the reason is the finding

With `meter hdr on`, the shell reported `meter TX header: AA 55`, three function
words were sent, and `echo_frames` stayed at 0. That was about to be recorded as
a failure to replicate. The `status` readback said otherwise:

```
last_tx_frame: 00 00 05 0B 00 00 00 00 00 10
tx_frames_recent: [00 00 05 08 …] [00 00 05 09 …] [00 00 05 0C …] …
```

**Every frame still began `00 00`.** The toggle reported a change that never
reached the wire.

**Root cause: the frame was built in two independent places.**
`usart2_send_cmd()` (the polled fallback) and a private copy inline in the
`dvom_TX` drain task (`fpga.c:2419`). Every `usart tx` goes through the queue,
so it hit the copy the toggle did not patch. Both copies carried the same wrong
comment blaming byte[8] `0xAA` for the zero echo frames.

Fixed by making `meter_build_tx_frame()` the single builder, called by both.

> This is the project's signature failure caught in four minutes instead of
> weeks: an instrument that could not have detected what it claimed to test,
> returning a stable, plausible, wrong answer. It was caught only because the
> transmitted bytes were read back rather than inferred from the toggle's own
> report. **A control that asks the instrument about itself is not a control.**

### 5b. Second run — CONFIRMED

Wire bytes verified in every phase. Three words sent per phase
(`05 0C`, `05 14`, `05 0B`):

| phase | `last_tx_frame` | echo delta | data delta |
|---|---|---|---|
| A1 baseline | `00 00 05 …` | **+0** | climbing |
| B1 | `AA 55 05 …` | **+3** | climbing |
| A2 | `00 00 05 …` | **+0** | +28 |
| B2 | `AA 55 05 …` | **+3** | +22 |
| A3 | `00 00 05 …` | **+0** | +27 |

Every echo in the B phases was **valid**: `echo_start=3 echo_hdr=3
echo_valid=3 echo_bad=0`. Three words, three echoes, byte[3] matching the word
sent and byte[7] = `0xAA` each time.

**`echo_frames` has never been non-zero in this project before today.**

### 5c. The modes actually take

With the header on, three commanded words produce three genuinely different
frames (probes open):

| word | frame | our decode |
|---|---|---|
| `05 0C` DC Voltage | `5A A5 F6 EB EB EB 07 00 82 00 00 EF` | `valid=1`, `0.0006 V` — correct for open probes |
| `05 0B` Resistance | `5A A5 04 F0 6B 01 00 24 00 00 00 F0` | `reject=1` — different family, decoder expects DCV |
| `05 0A` Capacitance | `5A A5 E4 FB EB EB 2B 10 00 00 00 F0` | `reject=1` |

The capacitance frame **independently reproduces Stlkv's decode from unit #2**:
he reported capacitance carries `f7 = 0x10` with the unit in the upper nibble of
`f6`, `0x2_` = nF. Measured here: byte[7] = `0x10`, byte[6] = `0x2B`, upper
nibble `2`. Different unit, different person, different method, byte-exact.

`live_same_submode` also flips 1 → 0 → 0 → 1 across DCV → R → C → DCV, an
independent signal that the incoming frame family changed.

## 6. Blind spots

- **The positive control exercises the `5A A5` branch, not the `AA 55` branch.**
  Data frames prove the ISR and sync machine run; they do not prove the echo
  counter can count. In the B phases the echo counters did move, so this gap is
  closed *for this result* — but a future zero-echo reading still needs
  `echo_start`/`echo_hdr` read separately to place the fault.
- **Only three words were tested** (`0x0C`, `0x14`, `0x0B`), plus `0x0A`. The
  other eight in Stlkv's map are untested here.
- **No known load was attached.** Probes were open throughout. This establishes
  that modes are *accepted*, not that any reading is *accurate*. DCV reading
  0.0006 V open is consistent, not a calibration.
- **Our decoder rejects every non-DCV family** (`reject=1`), so this says
  nothing about whether resistance or capacitance would decode correctly.
- **Unit #1 only**, one session, one build.
- The word table is still wrong for 10 of 11 submodes; this experiment
  deliberately bypassed it by sending words directly with `usart tx`.

## 7. Conclusion

**Established:**
1. The meter SoC requires the `AA 55` TX header. Confirmed on unit #1,
   reproducing Stlkv's unit #2 result. A/B/A/B/A, one variable, positive
   control holding in all five phases.
2. `echo_frames` is a working acknowledgement channel. The project now has the
   instrument it has been missing since EXP-05, and the two-week-old
   "`echo_frames` = 0, unexplained" entry is closed.
3. Commanded function changes are **accepted**: three words produce three
   distinct frame families.
4. The capacitance frame layout replicates across units.

**Excluded:** the byte[8] `0xAA` checksum explanation for the zero echo frames.
It was never measured and is now superseded. Withdrawn, not deleted.

**NOT established:** that any non-DCV reading is *correct*. Nothing was measured
against a known load. The decoder rejects non-DCV families, so accuracy is
EXP-26 and onward, and it needs Stlkv's PR (2).

**Also established, unexpectedly:** our meter frame had two independent
builders, and a header fix that patches only `usart2_send_cmd` — the function
this project's own issue-#15 reply pointed Stlkv at — would have gone out on the
wire as `00 00` while reporting success. Flagged to him.
