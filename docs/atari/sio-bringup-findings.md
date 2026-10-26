# Atari SIO bring-up: what the hardware measurements showed

Findings from running fujinet-nio on a FujiNet v1 board (FN32ROV-1.6, ESP32-S3) against an
Atari 800XL, October 2026. Use it to know which timings are healthy and which design rules
came from real measurements. Re-measure before relying on a number for a different board.

## How to measure

`sio.reset` before powering the Atari, boot it, run the boot disk's FTESTNIO to the end, then
`sio.status` (see [diagnostics](../diagnostics.md)). Cold-boot both together with no USB cable
(the FujiNet powered from the Atari) as well as with the Atari power-cycled alone: the boot
timing differs. Logs show `[N ms] starting main loop` and `[N ms] phase 1 done in M ms`.

## Healthy numbers (D1 boot path, 19200 baud)

| Measure | Seen | Limit / meaning |
|---|---|---|
| Ingress ready | about 0.4-0.5 s after reset | before the Atari's first command |
| First real command | about 2.9-3.0 s after power-up, every run | set by the Atari, not the FujiNet |
| CMD release to first reply byte (`reply` max) | 9-10 ms quiet, 13 ms under a failing Wi-Fi connect before pinning | the Atari gives up after 16 ms |
| CMD release to start of processing (`react` max) | about 5 ms | |
| Handler | max 12-14 ms | slow work here delays the reply |
| Late ACKs, timeouts, UART errors | 0 | |

A 97-sector boot image is dominated by 256-byte sector transfers at 19200 baud (about 180 ms
each), not by FujiNet processing; single density or double density sectors are a bus-time
question, not a firmware one.

## What went wrong and why

1. **No ACKs at all.** The ESP-IDF call `gpio_set_direction()` on the TX pin re-routed it to a
   plain GPIO output and detached the UART. Never touch a UART-owned pin that way
   (`atari_sio_ingress.cpp`).
2. **The native 0x7f endpoint wedged after a timed-out call.** A native `W` runs its handler
   synchronously. The first Wi-Fi request on an unconfigured device initialised the radio
   inside that handler (a second or more), the Atari timed out (`$8A`), and the finished reply
   then sat in the `Ready` state, so every later `Q` or `W` was NAKed (`$8B`). Fixes:
   - Wi-Fi init and connect run in their own priority-1 task on core 0; the Wi-Fi service answers
     "not ready" until the link is published (`main_esp32.cpp`).
   - A reply left unread for 2 s is discarded by the next `Q` or `W` (`AtariSioNioFrontend`).
     `P` and `R` never discard it. This is covered by unit tests only; the original trigger is
     gone, so it cannot be provoked on hardware.
3. **The SIO task was starved by the radio.** With a failing Wi-Fi connect the worst reply was
   13.3 ms against the 16 ms deadline. Pinning the core/SIO task to core 1 (the Wi-Fi driver is
   pinned to core 0) brought it to 9.2 ms with the same bad SSID. The CMD interrupt and SIO
   UART are installed by that task, so they run on core 1 too.

## Misreadings to avoid

- The "first CMD edge" in `sio.status` is often not a command. With Wi-Fi active a transient
  appears around 1.5-1.7 s, long before the Atari's first command at about 2.9 s. An early first
  edge with a much later first frame is not by itself evidence of missed commands; check `react`
  and `reply`.
- `aborts edge` counts extra CMD edges between polls. It is 0 with Wi-Fi off, about 6 during a
  failing connect and 100-150 with Wi-Fi connected and traffic flowing; it stays at 0 with
  Wi-Fi connected and the Atari idle. It tracks radio activity, so it looks like coupling onto
  the CMD input (GPIO39), not a firmware fault. Frames are still served (the Atari retries), so
  it costs a little time only. If it ever matters, look at the CMD input hardware (pull-up,
  filtering) before the firmware.
- The `poll gap` max counts only gaps during which a CMD edge occurred. It is consistently
  about 170 ms (240 ms with Wi-Fi off), which matches phase 1 (device registration) blocking the
  SIO task at about 1.4-1.6 s, overlapping the radio-start transient. The `phase 1 done in`
  log line measures it. The Atari's first command is about 1.3 s later, so nothing is lost on a
  cold boot; this would matter only if the Atari started talking sooner.

## Design rules this implies

- Nothing that can take more than a few milliseconds may run on the SIO task, in a handler or in
  startup. Bring-up that touches the radio, network or flash belongs on another task.
- Do not change anything on the D1 boot path without re-running the numbers above; a request
  slower than 16 ms makes the Atari retry or fail.
- A native call that takes long must answer "not ready"/busy at once, not block.

## Open items

- Split phase 1 so no single tick runs long (confirm with the `phase 1 done in` line first).
- High-speed SIO (HSIO) has not been measured; the numbers above are at 19200 baud.
- FTESTNIO crashes with the BASIC cartridge enabled; boot it with BASIC off (it ends on the
  SELF TEST screen).
