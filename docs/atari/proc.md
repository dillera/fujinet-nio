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

Second run: poll mode then counted far too many (2,964 for 25 pulses; 133, 420 and 686 for runs of 5, growing
with how long the program ran before the reset). The cause was in the program, not the line: the clearing
read of `PORTA` was written in C as an unused read, and cc65 removes an unused read of anything that is not
`volatile` (the PIA struct in `<atari.h>` is not), so the flag was never cleared and was counted on every
pass of the loop. The library's handler clears it in assembly, which is why interrupt mode counted 25 of 25
throughout. PROCTEST now reads `PORTA` through a volatile pointer (checked in the compiler listing).

## Using it: the data-ready pulse

PROC is a hint that **a read that would have been refused (`NotReady`) would now succeed**, so a program can
carry on with its screen and its own work and look when told, instead of repeating the read. It is
optional at every level:

- The firmware pulses PROC only when `sio.tune proc 1` (or `proc_notify: true` under `atari_sio:` in
  `fujinet.yaml`, saved with `sio.save`) is set. The default is off, and Q flag **bit 1** advertises it only
  while it is on (bit 0 is "R may ask for more than the response").
- The Atari's PIA interrupt stays off until a program calls `fn_atari_proc_enable()`; the module is linked only
  by a program that calls it.
- Polling is always correct. A pulse can be early, repeated, or for data another request already took, and a
  missed pulse costs nothing because the data stays buffered.

What triggers it today: a **TCP** session of the client (`tcp://`, the native `NetworkDevice`) that has bytes
buffered, or whose peer closed or failed. The network service checks each session on its poll tick
(`INetworkProtocol::read_ready()`, only TCP implements it) and tells the client that owns the session
(`IClientNotifier`, `IRequestHandler::watchClient`) once when it turns readable. A `Read` that leaves data
behind clears the mark, so it is announced again at the next tick and a program that reads part and then
waits is not stranded; a read that takes everything, or reaches end of stream, adds no repeat. HTTP and TLS sessions do not report readiness yet; their clients keep polling.

The ingress turns a notice into a pulse only when the Atari is idle (CMD high, no frame or write data being
received), holds PROC low for 500 us, and makes pulses at least 5 ms apart (notices closer together share
one). `sio.status` shows `proc: ... notify on: N readable notices, M pulses`.

How a program uses it (`fujinet-nio-lib`):

```c
fn_init();
fn_atari_query_capabilities();                    /* optional: learn whether the firmware pulses */
use_proc = (fn_atari_capability_flags() & FN_ATARI_CAP_PROC) != 0;
if (use_proc) fn_atari_proc_enable();

/* send the request, then carry on with the screen ... */
for (;;) {
    update_screen();
    if (!use_proc || fn_atari_proc_pending()) {   /* told, or polling */
        err = fn_read(handle, offset, buf, len, &got, &flags);
        if (err == FN_OK) { consume(buf, got); ... }
        /* FN_ERR_NOT_READY: nothing yet, fine */
    }
}
```

A simple program skips PROC and calls `fn_read` in a loop, handling `FN_ERR_NOT_READY`.

## Not done yet

- **A request that is slow to answer.** The firmware runs a request inside the `W`, so the Atari's SIO call
  waits for it, and the response is ready the moment `W` completes. A PROC pulse for "response ready" would
  say nothing new. Letting `W` return at once and finish the request afterwards (the spec's `pending` state)
  is a separate change: the request would still run on the SIO task, so the Atari could not use SIO until it
  is ready.
- **HTTP and TLS readiness**, and other services with a delayed answer (the same hook, `read_ready()`).
- **The first hardware proof** of the data-ready pulse itself with a real TCP session.
