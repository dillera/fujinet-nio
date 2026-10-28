# The PROC (proceed) line

PROC is how a SIO device tells the Atari "something is ready" without the Atari polling for it. It is SIO
connector pin 9, an open-drain output on the FujiNet (ESP32 GPIO22 on the v1 boards, `pinmap.cpp`), pulled
up on the Atari side: **low is asserted**.

## The Atari side

- The line goes to the PIA's CA1 input. A **falling** edge sets the CA1 flag (`PACTL` $D302, bit 7); with
  `PACTL` bit 0 set the PIA also raises an IRQ, and the OS IRQ routine calls the handler at **`VPRCED`**
  ($0202) with A on the stack (it must end with `PLA`, `RTI`). Reading `PORTA` ($D300) clears the flag.
  `PACTL` bit 1 selects the edge (0 = falling, the power-on state).
- The OS leaves the interrupt disabled. The SIO routine rewrites `PACTL` when it switches the cassette
  motor, which clears the enable bit.
- The sister line INTERRUPT (pin 13, CB1, `VINTER` $0204) is not used by the FujiNet.

`fn_atari_proc_enable()` in `fujinet-nio-lib` does the Atari side: it installs a `VPRCED` handler that counts
the interrupt, sets a pending flag and chains to the handler that was there, and sets `PACTL` bit 0.
`fn_atari_proc_pending()` and `fn_atari_proc_count()` read what it recorded; `fn_atari_proc_disable()` puts
everything back. It is opt-in and links nothing into a program that does not call it. The cycle of the
handler and the enable/disable/chain logic are tested in `make check` (`atari_proc_soft65c02`), with RAM
standing in for the PIA.

## What the firmware does today

**Nothing in the protocol drives PROC yet**: the ingress configures GPIO22 released (high) and leaves it, and
the native endpoint is polling only (`P`), as the Epic 3 readiness notes say. The only thing that moves the
line is the diagnostics command:

```
sio.proc                       show the line and how often it was driven low
sio.proc low | high            assert (drive low) or release
sio.proc pulse [n [low ms [high ms]]]   n pulses (default 1 x 10 ms low, 10 ms high)
```

A pulse blocks the SIO task for its length (at most 100 x 1000 ms, so keep it short), so run it with the
Atari idle. `sio.status` shows the line as `proc: released (high), driven low N times`.

## Proving the path on the hardware

PROCTEST (nio-core-apps, `apps/platform/atari/proctest.c`) checks the two Atari layers separately. Build a
boot disk with `make TARGET=atari boot-disk BOOT_DISK_MANIFEST=boot-disk/manifests/atari-proctest.yaml
BOOT_DISK_OUTPUT_DIR=build/atari/disk/boot-proctest`, flash it and the firmware, boot the Atari, then on the
FujiNet console run `sio.proc pulse 5`:

| Mode | What it counts | If it stays at 0 |
|---|---|---|
| **P** (default) | the PIA's CA1 flag, polled from the main loop, no interrupt | the signal does not reach the PIA: the pin, the open-drain drive, the cable or the board (check GPIO22 and SIO pin 9 with a meter; `sio.proc low` should hold the line at 0 V) |
| **I** | interrupts delivered to the library's `VPRCED` handler | the flag works but the interrupt does not: `PACTL` bit 0, the vector, or a program that rewrote them |

## Hardware result (PROCTEST, first run)

`sio.proc pulse 5` (10 ms low, 10 ms high) in **interrupt mode** gave `interrupts: 5`: the pin drive, the wire to
SIO pin 9, the PIA edge, the OS IRQ, `VPRCED` and the library handler all work, one interrupt per pulse. In
**poll mode** it showed 2 of the 5: the CA1 flag is one latch, and that version redrew the screen (longer than
the 20 ms between pulses) after each flag it cleared, so pulses 2 to 5 merged. The program now counts in a
tight loop and redraws only after the edges stop. The "flag set for about a second" and "pending for about
five seconds" seen were the old redraw interval, not the hardware.

## Not done yet: using it for the protocol

The point of PROC for NIO is that a client can wait for a response (or for data a service has pending) on an
interrupt instead of polling `P`. That needs decisions that are not made here: when the firmware asserts
(a response became ready after `W`; data became available on a network handle), when it releases (when the
`R` that consumes it completes), a Q flag bit so a client knows it can rely on it, and a library call such as
a bounded wait on the pending flag. Until the line is proved, none of that is built.
