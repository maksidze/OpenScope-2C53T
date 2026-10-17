# What breaks when someone finally answers

*2026-09-12*

For five months this project's multimeter worked. DC volts read correctly.
Resistance read correctly. We built a submode table, a frontend posture plan, a
transition state machine and a poll task on top of it, and bench-confirmed
coexistence with the scope only eight days ago.

None of it had ever been received. Every command this firmware sent to the meter
was discarded before it was parsed, for the entire life of the project, and the
readings were correct because the meter's own power-on **auto mode** happens to
measure volts and ohms.

Tonight we made it listen. Almost everything we had built on top of not being
heard came apart within twenty minutes, and the four bugs that fell out were
mostly in our instruments rather than in the hardware.

## The two bytes

The finding is [@Stlkv](https://github.com/Stlkv)'s, on [issue
#15](https://github.com/DavidClawson/OpenScope-2C53T/issues/15), measured on his
unit #2 five days ago. Meter TX frames must begin `AA 55`. Ours begin `00 00`.
With the wrong header the meter SoC stays silent and holds auto mode.

He did not find it with a logic analyzer. He patched a code cave into stock
V1.2.0 itself, hooked the dvom TX task and SysTick, wrote every frame into an
SRAM block above stock's stack, and read it back through a MENU+Power round trip
into his own firmware. That method is worth more than the result, and the result
is worth a lot.

It explained something we had been carrying as unexplained since EXP-05:
`echo_frames` had been **0** on every build for weeks. We had a wrong
explanation sitting in our own source for months, blaming a `0xAA` at byte 8 for
checksum failures. It sounded plausible. It had never been measured.

## Making the change A/B-able

The header went in as a **runtime toggle**, not a compile-time flag: `meter hdr
on|off`, defaulting **off**. That matters for two reasons. The comparison runs
A/B/A inside one boot with the same probe and the same cell, so nothing drifts
between phases. And the device still boots bit-identical to the one that took
every earlier measurement, so the baseline stays a measurement rather than a
memory.

Then the first run said the header does not replicate. Header on, three function
words sent, `echo_frames` still 0.

That was very nearly the session's conclusion.

## The builder we had two of

The only reason it wasn't is a habit this project learned expensively: read back
what actually happened, not what the thing that made the change says about
itself.

```
last_tx_frame: 00 00 05 0B 00 00 00 00 00 10
```

Every frame still began `00 00` while the shell cheerfully reported `AA 55`. The
ten bytes were assembled in **two independent places** — `usart2_send_cmd()`, and
a private copy inline in the `dvom_TX` drain task. Both carried the same wrong
byte-8 comment. Every `usart tx` goes through the queue, so every test frame hit
the copy the toggle had not patched.

There is now one builder and both paths call it.

This one reaches past us. Our own reply on #15 pointed Stlkv at
`usart2_send_cmd` **by name**. Had he patched only that, his header fix would
have shipped reporting success while sending `00 00`, and his bench could not
have caught it, because he drives the meter by hand rather than through our task.

## It replicates

With one builder, the A/B/A/B/A is unambiguous. Three words per phase:

| header | echoes |
|---|---|
| `00 00` | 0 |
| `AA 55` | **3** |
| `00 00` | 0 |
| `AA 55` | **3** |
| `00 00` | 0 |

All valid, none bad. Data frames climbed in all five phases, so the receive path
was proven working throughout rather than assumed — the positive control this
question has never had.

`echo_frames` had never been non-zero in this project before tonight.

## Then the relays started clicking

Within minutes the unit was clicking continuously and audibly from across the
room.

`fpga_meter_poll_task` sends the stock start word every 250 ms. It has done that
since April. Every one of those was discarded, so the cadence had never been
exercised against a meter that obeys. The moment the header was right, all four
per second were accepted, and each one re-triggered a measurement and an
autorange. Real relay actuation, real mechanical wear, on a user's instrument.

**Our poll rate was only ever safe because nothing was listening.**

The obvious fix, slowing the poll, only makes the clicking slower. So the real
question was whether the poll is needed at all, and the measurement is pretty:

| condition | TX | data frames |
|---|---|---|
| poll running, header off, **zero commands accepted** | 4.1/s | **6.0/s** |
| transmit stopped entirely | 0.0/s | **0.0/s** |

I got this wrong in the moment — called it free-running on the strength of the
6-versus-4 mismatch, then stopped transmitting and watched data go to zero. The
truth is better: the meter answers **USART traffic**, not commands it
understands. It hears the bytes and replies; it only *obeys* a frame with the
right header.

Which retires, in its stated form, a line `CLAUDE.md` has carried since April:
the meter "only emits data frames in response to recent TX commands." Correct as
an observation. Its wording implies the commands were understood. They were not,
and that sentence is a good part of why nobody questioned the meter for five
months.

So the keepalive now goes out deliberately **unobeyed**, with the old `00 00`
header, carrying an OBEY bit in the transmit queue item so the poll task and the
shell cannot race a shared flag. Real commands carry `AA 55`. Verified both
ways, because a gate that silenced the keepalive by breaking commands would pass
half the test: one explicit command gives one echo, sixteen keepalive frames
after it give none.

## The validator that raced its own keepalive

Commanding resistance then reported *not accepted*. The frames had visibly
changed family, so something was wrong with us, not the meter.

The echo ladder answered it in one read: `echo_start +1`, `echo_hdr +1`, `valid
+0`, `bad +1`. The echo **arrived and was rejected**. Validation compared byte 3
against `last_tx_frame`, which the 4 Hz keepalive overwrites in the 250 ms
before an echo lands, so a perfectly good `0x0B` echo was checked against the
keepalive's `0x09`.

Worth dwelling on: with a single `echo_frames` counter this would have read
exactly like "the meter refused." The instrument that found the bug was the
decision, made before any measurement, to count each rung separately.

## Numbers

With that fixed, and the meter finally in modes we asked for:

| measurement | result |
|---|---|
| DC volts, commanded, n=12 | 1.6144 V, spread 0.0001 V |
| the same cell in auto mode (EXP-23) | 1.6158 V, **−0.088%** |
| shorted probes, resistance | 0.014 Ω |
| 10 kΩ ±5% | **9.775 kΩ**, −2.25% |

The resistance readings are hand-decoded from raw frames using Stlkv's rule that
frames are seven-segment text with bit 4 of a digit as its decimal point. **His
decode rule replicates on our unit across two decades.** His leads spelled
`" 0.17"`; ours spell `0.014`. Same encoding, different leads, both physically
sensible.

And a third independent kill for the withdrawn 0.0304 resistance factor: multiply
9775 by it and you get 297, which is not the resistance of anything on this bench.

One thing we will not claim. The resistor is known only to ±5%, so the reading
falls inside its band. That is not the same as the meter being accurate to
2.25%, because the reference is looser than the measurement.

The scope, meanwhile, never noticed: 15.7 reads/s before the meter cycle, 16.0
after, with CH1 tracking the generator linearly over a 6:1 amplitude range.

## What this was really about

Three of tonight's four defects were instruments. A frame builder that existed
twice, so the fix never reached the wire. A poll cadence that was safe only while
nothing obeyed it. A validator racing its own keepalive. Each returned a stable,
plausible, wrong answer — the failure this project keeps meeting, and the reason
every experiment here carries a control and a written blind-spot list.

There is a pattern across the year worth naming, because this is the fourth
instance. The timebase button moved a variable nothing read. The word table
selected functions nothing obeyed. The poll ran a cadence nothing acted on. The
header reported a change nothing transmitted. **Each looked correct precisely
because it was inert**, and each was only exposed when something on the other end
finally answered.

Anything this firmware drives should be assumed untested until something has
demonstrably replied.

*Evidence: EXP-25 through EXP-28 in `docs/experiments/`. The header finding, the
word map and the decode rule are Stlkv's, on issue #15, measured on a second
unit before we ever touched ours.*
