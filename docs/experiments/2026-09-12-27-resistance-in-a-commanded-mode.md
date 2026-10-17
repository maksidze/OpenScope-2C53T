# EXP-27 — Resistance in a mode we actually asked for

- **Date:** 2026-09-12
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace-meter`, branch `bench/2026-09-12`
- **Status:** **CONFIRMED**

## 1. Problem

EXP-25 showed the meter accepts commanded modes. EXP-26 showed commanded DC
Voltage agrees with auto mode. Neither shows a **non-DCV** function producing a
correct number, which is the thing nobody has ever demonstrated on this project
(spec `meter-in-the-scope-build`, open question 2).

## 2. Hypothesis

With the `AA 55` header, `05 0B` puts the meter in resistance and its frames
carry a real measurement.

- **If true:** shorted probes read a small non-zero lead resistance, and a
  known resistor reads within its tolerance band.
- **If false:** the frames are identical regardless of what is across the leads.

That falsifier is not hypothetical — it is what the first attempt looked like.

## 3. Procedure

```
mode meter ; fpga usart on ; meter hdr on
usart tx 05 0B          # Resistance, from Stlkv's measured word map
meter dump              # raw frame + digit fields, sampled ~12x over ~10 s
```

Our decoder rejects the resistance family (`reject=1`), so the reading is
**hand-decoded from raw frame bytes** using Stlkv's rule: frames are
seven-segment text, and bit 4 of a digit is its decimal point.

**Preconditions verified by readback:**

| what | expected | measured |
|---|---|---|
| command accepted | echo +1, bad +0 | **+1 / +0** ✅ |
| header on the wire for commands | `AA 55` | ✅ |
| keepalive stays unobeyed | `00 00` | ✅ |
| cell removed before commanding resistance | — | operator-confirmed |

## 4. Control

**Shorted probes**, same session, same path, recorded first.

| | digits | decoded |
|---|---|---|
| x6 | `10,00,01,03` | 0.013 Ω |
| x5 | `10,00,01,04` | **0.014 Ω** |
| x1 | `10,00,01,05` | 0.015 Ω |

`0x10` on the leading digit is the decimal point → `0. 0 1 4`. 13-15 mΩ is an
ordinary test-lead resistance. The last digit dithers while `f6` rotates
0F/0E/07, so this is a live measurement with noise, not a replayed constant.

Stlkv's unit spelled `" 0.17"` (0.17 Ω) on his own leads. Different value, same
encoding. **His decode rule holds on a second unit.**

## 5. Results

**10 kΩ ±5% (brown-black-orange-gold) across the leads:**

| | digits | f6 | decoded |
|---|---|---|---|
| x6 | `09,07,07,05` | `47` | **9.775 kΩ** |
| x4 | `09,07,07,06` | `47` | 9.776 kΩ |
| x3 | `09,07,07,04` | `4E` | 9.774 kΩ |

No decimal-point bit on any digit → the text is `9775`. `f6` upper nibble `4` is
the kΩ regime (`CLAUDE.md`: upper nibble 4 → kΩ), giving **9.775 kΩ**.

- **−2.25% from nominal**, comfortably inside the gold ±5% band.
- The frames change completely between shorted and 10 kΩ — bytes[2..5] go
  `04 E0 1B 4A` → `C4 9F 8A CA`. The falsifier is cleanly not met.

**A third independent kill for the withdrawn 0.0304 factor:** 9775 × 0.0304 =
297, which is not a resistance of anything on this bench.

## 6. Blind spots

- **The resistor is only known to ±5%.** We can say the reading falls inside its
  band and is physically sensible. We **cannot** claim the meter is accurate to
  2.25%: the reference is looser than the measurement. A traceable resistor
  would be needed to say more, and this is the same gap as `SCOPE_CAL_SOURCE_SCALE`.
- **One resistor, one decade.** Nothing here tests the low-Ω band, the MΩ band,
  or the band-switching behaviour that caused the original flicker bug.
- **Hand-decoded.** Our firmware still rejects these frames; the decode lives in
  this document, not in the tree. Until the decoder lands, nothing regression-tests it.
- **No reference meter** was placed across the same resistor in the same session.
- Lead resistance was not subtracted from the 10 kΩ figure; at 14 mΩ it is
  0.00014 of the reading and irrelevant here, but it will not be in the low-Ω band.

## 7. Conclusion

**Established:** a non-DCV meter function produces a correct reading in a mode
this firmware explicitly commanded. Shorted probes read 14 mΩ, a 10 kΩ ±5%
resistor reads 9.775 kΩ, and the frames change completely with the load. That
answers open question 2 of the meter spec affirmatively for the first time.

**Established for someone else's benefit:** Stlkv's seven-segment decode rule,
including the decimal-point bit and the `f6` upper-nibble unit regime,
replicates on unit #1 across two decades. His PR (2) can be written against
these fixtures rather than against his own unit alone.

**NOT established:** meter accuracy in any calibrated sense, any other band, or
anything at all about the eight untested words in the map.
