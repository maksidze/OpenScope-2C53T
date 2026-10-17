# Spec: Multimeter alive in the scope build

**Track:** meter
**Stage now:** **S1 in `guest-coldtrace`, all submodes accepted** (2026-09-12). Coexistence works —
scope and meter run in one image at the same time (EXP-23, 2026-09-04), and the
submodes became *reachable* in EXP-24 (2026-09-04). They are still not
*accepted*: Stlkv measured on unit #2 that meter TX frames must begin `AA 55`
and ours begin `00 00`, so the meter SoC has discarded every command this
project has ever sent and has been running its own power-on auto mode
throughout (2026-09-07, issue #15). DC Voltage read correctly because auto mode
measures DC Voltage. This is issue #15.
**Champion:** —

## What it is

One image where the mode button takes you from a live scope to a working
multimeter and back, on **every** meter function.

Coexistence itself is done. In `guest-coldtrace` a mode switch to meter raises
PC11, brings USART2 up, and DCV reads a cell correctly while SPI3 acquisition
continues (EXP-05 2026-08-17, re-confirmed EXP-23 2026-09-04: 1.6158 V against a
1.61 V reference, `SPI3 OK` climbing throughout).

What is missing is **submode selection**. `FPGA_WARM_HANDOFF_TEST` does double
duty — it means both "the FPGA is already configured, skip config" *and* "this
is a scope-only image, no-op every meter entry point" — so
`fpga_set_meter_mode()` returns before its first statement. Resistance,
continuity, diode, current and capacitance all update the UI label and reach the
hardware not at all: DCV vs Resistance gives a bit-identical GPIO posture and an
identical TX frame (EXP-23, with a passing positive control that the same
instrument *does* see relay changes). The build works on DCV because that is
the posture it happens to boot in.

## Prior art

Stock does both in one image, trivially — which is exactly why this gap is the
first thing a stock user would notice. Wishlist Tier 1 #2 (manual range lock)
and the >10 V decimal-latch bug are the next two meter asks, but both are
moot until the meter runs in the build people actually use.

## Our angle

Once coexistence works, our meter already beats stock's documented annoyances
in reach: the decoder is ours (band-level dp fix, stable low-Ω/kΩ readings),
so range lock and honest resolution are firmware-sized follow-ons, not
reverse-engineering projects. Each gets its own spec once this one is at S2.

## Hardware dependencies

**There is no "kill" to explain.** Both hypotheses this section used to carry —
that the uploaded bitstream removes a meter-only NV design, and that the meter
might not be the FPGA at all — were answers to a question that turned out to be
malformed. The meter was never killed by FPGA configuration; it was dead because
our runtime drove PC11 (meter MUX enable) LOW unconditionally and never applied
stock's per-mode GPIO posture. Fixed 2026-08-17 (`ca7f28b`, `7326eac`).

What remains is ordinary firmware work on our side:

- Stock applies a **mode-entry sequence** — pins, then bus, then commands
  (`0x0800E360` enter / `0x0800E3E4` exit). We now do pins and bus; the commands
  half is what `fpga_set_meter_mode()` would send if it ran.
- The meter analog frontend is never postured for a submode in this build:
  `fpga_set_meter_frontend_for_submode()` has no reachable caller under
  `FPGA_WARM_HANDOFF_TEST`.
- `fpga_init` returns early in coldtrace, before its meter-frontend and
  meter-USART steps, so the boot-time posture is scope's.
- `echo_frames` stays 0 — data frames arrive, echo frames never do. Open since
  EXP-05, unexplained, and worth resolving before trusting any command
  acknowledgement.

## Stage ladder

| To reach | Criterion (checkable) |
|---|---|
| ~~S1 (in coldtrace)~~ | **MET** — EXP-05 (2026-08-17), re-confirmed EXP-23 (2026-09-04). DCV tracks a bench source in `guest-coldtrace` with the scope still capturing. |
| ~~S1 (all submodes, *reachable*)~~ | **MET** — EXP-24 (2026-09-04). `fpga_set_meter_mode()` runs here; DCV → Resistance moves the frontend posture (`gpio scan` diff non-empty) *and* changes the TX frame. |
| ~~S1 (all submodes, *accepted*)~~ | **MET** — EXP-25/27 (2026-09-12). `echo_frames` moves off zero only with the `AA 55` header (A/B/A/B/A, 3 words per phase, 0/3/0/3/0, all valid), and a commanded non-DCV function reads a known load: 10 kΩ ±5% → 9.775 kΩ, shorted leads → 0.014 Ω. |
| S2 | DCV 0–9 V and resistance re-verified within a few percent **against a bench DMM** *post-config*, same session writeup. ⚠ **NOT met by EXP-26/27.** Those measured 1.6144 V against a cell and 9.775 kΩ against a ±5% resistor — both inside their bands and physically sensible, but neither is a reference: the resistor is known more loosely than the reading it is checking. Same gap as `SCOPE_CAL_SOURCE_SCALE` on the vertical axis. Needs a trusted meter across the same load in the same session. |
| S3 | Host regression over captured USART frames for the decoder (exists in part); on-device `bench.py` acceptance: scope→meter→scope cycle with a live reading at each stop. |
| S4 | Range lock (wishlist #2), >10 V dp fix, honest trailing digits (wishlist #4) — each promoted through its own spec. |

## Open questions

1. ~~**Why does `echo_frames` stay 0?**~~ **ANSWERED 2026-09-07 by Stlkv
   (issue #15), measured on unit #2.** Meter TX frames must begin `AA 55`;
   `usart2_send_cmd()` leaves bytes 0 and 1 at zero. With `00 00` the SoC stays
   silent and holds its power-on auto mode; with `AA 55` it echoes every
   accepted word. Open since EXP-05, and carried in our own source with a wrong
   explanation blaming a `0xAA` at byte[8] for checksum failures.
2. ~~Do the non-DCV submodes actually work, or only become reachable?~~
   **ANSWERED, negatively.** EXP-24 established reachable. The header result
   establishes that none was ever accepted, so the answer for every session up
   to 2026-09-07 is that they did not work and could not have.
3. ~~Should `FPGA_WARM_HANDOFF_TEST` be split?~~ **DONE** — `56b258c` split it
   on `FPGA_METER_SUBMODES`.
4. **Which word does each submode actually need?** Stlkv's logger recovered the
   full twelve-word stock map. Cross-checked against
   `fpga_meter_stock_mode_for_submode()`, **10 of our 11 submodes send the
   wrong word**; only Temperature is right, and by coincidence. Submode 0
   "DCV" sends `0x14`, which is stock's *Auto*. The eight bytes we recovered by
   reverse engineering all appear in his twelve, so the extraction was sound and
   the function assignment was not. His four extra words (`0x0D`, `0x0E`,
   `0x15`, `0x16`) are exactly the ones whose absence forced the shared-slot
   fallbacks in `fpga_meter_plan.c`, so those can go.
5. **Does the header result replicate on unit #1?** Not yet measured here. The
   positive control is `echo_frames` moving off zero, which this project has
   never once observed.
