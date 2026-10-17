# EXP-28 — The scope survives a session of meter work

- **Date:** 2026-09-12
- **Unit:** bench unit #1
- **Build:** `make guest-coldtrace-meter`, branch `bench/2026-09-12`
- **Status:** **CONFIRMED**

## 1. Problem

EXP-25/26/27 changed the meter TX path, added a keepalive, and drove the meter
through DCV, resistance and capacitance — all of which re-posture the **shared**
analog frontend (PC12, PE4/5/6, PA15, PA10, PB10, PB11). Does scope acquisition
still work afterwards? This is the coexistence claim M1 rests on.

## 2. Hypothesis

Meter mode changes do not damage scope acquisition.

- **If true:** SPI3 reads continue at the same rate across a
  scope → meter → scope cycle, and CH1 still tracks a commanded amplitude.
- **If false:** the read rate drops or stalls, or the capture stops responding
  to the source.

## 3. Procedure

```
mode scope / mode meter / mode scope     # the cycle
status                                   # SPI3 OK delta over 6 s windows
spi3 opread 04 / 05                      # 1024-sample windows
```

Source: **JDS6600**, both channels 1 kHz sine, 2.0 Vpp, 0 V offset, outputs on.
Every setter is readback-verified by the driver, so a silently-refused write
cannot masquerade as a signal.

## 4. Control

SPI3 read rate measured **before** the cycle, in the same session, after all of
tonight's meter work had already run.

| phase | SPI3 OK |
|---|---|
| A: scope, control | 15.7/s |
| B: meter mode, header on, keepalive running | 15.7/s |
| C: back to scope | **16.0/s** |

Unchanged through the cycle, including while the meter was actively being
polled. Meter data frames continued climbing in phase B (1728 → 1763), so both
subsystems were genuinely live at once rather than one being idle.

## 5. Results

**CH1 capture, 1024-sample windows:**

| commanded | span (counts) | counts/Vpp |
|---|---|---|
| 0.5 Vpp | 14 | 28.0 |
| 1.0 Vpp | 27 | 27.0 |
| 2.0 Vpp | 52 | 26.0 |
| 3.0 Vpp | 77 | 25.7 |

Monotonic and linear to about 8% across a 6:1 amplitude range, with the mean
holding at 81-84 counts. The scope is capturing a real, source-tracking signal
after a full session of meter work.

**CH2 read `0x05` returned all zeros** (n=1024, span 0). This is the
**pre-existing CH2 gap**, not a regression from tonight: CH2 has had one usable
attenuator tap since 2026-08-17 and its offset reference (TMR13/PA6) is decoded
but never programmed in this build. Recorded so the zero is not mistaken later
for something this session caused.

## 6. Blind spots

- **CH2 was not actually tested.** Its read returns zeros for known reasons, so
  this experiment says nothing about whether meter work affects CH2.
- **No before-measurement from earlier in the session.** The control is "after
  the meter work, in scope mode", not "before any meter work at all", because
  the unit was reflashed three times tonight. A defect present from the first
  flash would not be caught. EXP-24's coexistence result is the earlier anchor.
- **Amplitude linearity is not a calibration** — the JDS6600's absolute output
  was not checked against a reference, and `SCOPE_CAL_SOURCE_SCALE` still
  applies to any volts claim.
- **One frequency** (1 kHz) and one timebase. Nothing about the acquisition edge
  seam, which is a separate open defect.
- The frontend restore was verified by **outcome** (capture works) rather than
  by diffing pin state, so a restored-but-different posture that still captures
  would pass.

## 7. Conclusion

**Established:** scope acquisition survives a scope → meter → scope cycle with
the new TX path, the keepalive, and live mode switching. SPI3 read rate is
unchanged and CH1 tracks commanded amplitude linearly. Meter and scope were live
simultaneously.

**NOT established:** anything about CH2, absolute vertical scale, or behaviour
at other timebases.

With EXP-25 through EXP-28, the meter half of M1 is measured: the header
replicates, commanded modes are accepted, a non-DCV function reads correctly,
and the scope is undisturbed.
