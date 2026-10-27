# High-speed SIO (HSIO) on the Atari

The trials, measurements and the justification for each timing value are in [hsio-report.md](hsio-report.md).

Status: implemented for `3F` on `D1:`-`D8:`, off by default, switched on at run time with the console
command `sio.hsio <0-9>` (not saved; there is no persistent setting or native-endpoint command yet).
Off, `3F` is NAKed and the Atari stays at 19200 baud. This records how the legacy firmware does it,
what the MyPicoDOS driver expects, and what is and is not built. Measured numbers are in
[sio-bringup-findings](sio-bringup-findings.md).

## How the legacy firmware does it

Source: `fujinet-firmware`, `lib/bus/sio/sio.cpp`, `sio.h`, `lib/device/sio/{disk,sioFuji,sioNetwork}.cpp`.

- **Index to baud.** POKEY divides the machine clock: rate = clock / (2 x (index + 7)). The legacy
  ESP32 target is `1773447 x 10 / (10 x 2 x (index + 7) + 3)` (PAL clock, plus 3 CPU cycles per byte
  for late POKEY sampling). Index 0 is about 124 kbaud, 6 about 67 kbaud, 10 about 52 kbaud.
- **`$E3` Set HSIO Index** (device `$70`, the FujiNet's configuration device): AUX1 is the index
  (0-10), AUX2 = 1 saves it to the config file. The wiki notes the Atari usually needs a reboot,
  because most SIO routines read the speed once. Default is disabled, reported as index 40
  (19200).
- **`$3F` Get HSIO Index.** The legacy firmware answers it on the disk devices, the network device
  and `$70`. On a disk or network device it sends the index byte at 19200 and then switches the UART
  to the high baud for everything after, including receiving command frames. On `$70` it only
  reports the index. A disk answers only if the image allows HSIO (ATX images do not).
- **High-speed disk commands.** The disk device (`$31`-`$38`) also accepts the normal command with
  bit 7 set: `$D2` read, `$D3` status, `$D7` write, `$D0` put, `$A1`/`$A2` format. These are disk
  commands. The same byte values on `$70` are unrelated (Get Time, Random Number, Mount All).
- **After COMPLETE at high speed** it flushes and waits 1 ms before the data frame: SpartaDOS X's HSIO
  driver misses a data frame that follows COMPLETE immediately, and a real drive leaves a gap.
- **Recovery from a speed mismatch.** Two command frames in a row with a bad checksum toggle
  between 19200 and the high speed.

## The Atari side

- With a U1MB the SIO driver can be forced to HSIO for chosen devices ("SIO Driver" HSIO, "SIO
  devices" All, per-drive driver for D1:-D4:; see the U1MB firmware manual, page 27). The
  maintainer runs it that way. nio NAKs `3F` today and still boots at 19200, so a forced HSIO driver
  falls back on a NAK.
- Without a U1MB the program needs a high-speed SIO driver of its own (MyPicoDOS's author's code is
  in `fujinet-build-tools/AtariSIO/tools/`, as embedded 6502 images; how it picks a speed was not
  worked out).

## What the MyPicoDOS driver does (repos/MyPicoDOS/hisio)

- **`3F` is the Happy/Speedy "ultra speed" query.** It is sent once per drive unit at the standard
  speed (a 1-byte read, AUX1 `$20`). The reply byte is the POKEY divisor: `AUDF3` = byte, `AUDF4` = 0,
  `AUDCTL` = `$28`, so the rate is clock / (2 x (n + 7)). `$28` (40) is the standard speed. The driver
  caches the speed per unit; unit 0 clears the table.
- **Everything runs at that speed:** the command frame, the ACK, COMPLETE and the data.
- **Fallback:** 14 command-frame retries at high speed, then a retry at the standard speed. The
  FujiNet therefore has to hear a standard-speed command frame again after switching.
- **A NAK on `3F`** sends it on to probe Turbo 1050 (status with AUX2 bit 7), XF551 (`$D3`) and Happy
  810 (`$48`). All fail against nio, so it falls back to the standard speed.
- **Do not implement the `$80` command variants.** `$D3` is how an XF551 is detected, after which data
  frames run at 38400 with the ACK still at the standard speed. Answering it would make a driver
  assume the wrong speed model.
- **Index 10 is not offered.** A speed byte of `$0A` is read as a Happy 1050 and probed further.
- The receive loop is hand-tuned for 126 kbit/s: the FujiNet's TX must be gapless (pacing is off).

## What is built

- `AtariSioSpeed` (`atari_sio_speed.h`): the index (-1 off, 0-9), the legacy baud for it, and whether the
  UART is at the high speed.
- `AtariSioDiskFrontend` answers `3F` with ACK, COMPLETE, the index and its checksum when an index is
  set (any AUX), and tells the router the reply is a high-speed answer. The router calls the ingress
  after the reply was sent, and the ingress then switches the SIO UART to the high baud.
- **Fallback:** `AtariSioCapture` counts command frames in a row that could not be read (bad
  checksum, UART error or overflow while collecting; edge and timeout aborts are noise and do not
  count). Two in a row while at high speed switch the UART back to 19200. This is what follows the
  Atari through a reset or its driver giving up.
- `sio.status` shows the `hsio:` state (index, current speed, switches, fallbacks, unreadable command
  frames); `sio.hsio` sets it; `sio.baud` checks the UART against the POKEY rates.
- There is no 1 ms gap between COMPLETE and the data frame. The legacy firmware added it for
  SpartaDOS X's own driver; this driver reads the data immediately. Add one only if a driver needs it.

## Still to do

Second result (`nofallback`): after the switch the Atari's command frames arrived at 19200 while we
listened at 67k (bits 3.5 times too wide: `0x38 0xc0 0xf8` and UART breaks). The legacy firmware
handles this by toggling the baud in both directions after 2 bad command frames
(`COMMAND_FRAME_SPEED_CHANGE_THRESHOLD`, `toggleBaudrate`), so the ingress now does the same. A frame at high speed read by a 19200 UART arrives as only one or
two garbage bytes, never five, so a command frame that ends (CMD released) with 1-4 bytes also counts
as unreadable; a CMD glitch with no bytes does not. The legacy log (an old run) shows the same
sequence: down after two bad frames, the Atari asks `3F` again at 19200, then up after two more.

Third result (toggle build): the toggle works but flaps (62 switches in 180 sectors); sectors take
either about 0.05 s or 0.2-0.3 s with bursts of UART breaks. The legacy firmware leaves gaps at high
speed that we did not: 250 us after ACK, and 1 ms from COMPLETE to the data frame (`DELAY_T5`,
`DELAY_HSIO_DATA`, "SpartaDOS X's own HSIO driver misses a data frame that follows COMPLETE straight
away"). The ingress now adds both when the UART is at high speed; at 19200 nothing changes.

Fourth result (gaps added): the boot is fast (112 sectors at about 50 ms each, one switch). After the
boot, the app's `0x7f` commands go out at 19200 because the driver only ever asked `3F` of `D1:`, so
we saw breaks and fell back (two commands lost to the library's retry). NOS calls the OS `SIOV` with
no speed logic of its own (`fujinet-nhandler/nos/src/nos.s`), so the speed a device gets is the SIO
driver's decision, and the legacy network device answers `3F` for that reason (`sioNetwork.cpp`).
The native endpoint now answers `3F` too (same reply, off by default). `sio.status` also shows the
first and latest refused frame for our devices and the latest frame for another device, which is how
to see what a driver asks that we do not have.

Portable and ESP-only: the `3F` replies, the refused-frame record and the router's high-speed hook are
in the shared frontends and router (host-tested). Only the UART baud switch, the bad-frame toggle and
the high-speed reply gaps are ESP32-only (`atari_sio_ingress.cpp`): the POSIX NetSIO carrier has no
baud, so its frontends keep the default (off, `3F` is NAKed). TODO: decide whether AltirraSDL (NetSIO)
needs `sio.hsio`-style setting to answer `3F` (an emulated HSIO driver would otherwise stay at standard
speed, which is correct but never exercises the high-speed path there).

Fifth result (`3F` answered on `0x7f`): no change. `nio(0x7f)` stayed at 13/25 (the app's own commands),
nothing was refused and no other device spoke, so the driver never sent `3F` to `0x7f` and sent its
commands at 19200. Index timings: 6 about 50 ms per boot sector, 1 about 33 ms, 0 about 30 ms.
In MyPicoDOS's HISIO driver the speed table is indexed by DUNIT (`SPEEDTB`, filled on the first command
to a unit by sending `3F` to that device) and `DUNIT = 0` clears it; the library calls `0x7f` with
DUNIT 1. The U1MB's own driver evidently differs, as 0x7f went at standard speed: which devices it
speeds up is its "SIO devices" setting. `sio.status` now counts `3F` queries per owner, and `sio.reset`
also clears the switch and fallback counts.
Idea, not built: route the SIO RX pin to two UARTs (19200 and high speed) and use whichever frame
checks out, which would remove the two-lost-commands cost when traffic alternates between D1: at high
speed and 0x7f at standard.

Sixth result (index 1, then U1MB's driver off): with the driver off nothing sends `3F` at all and the
boot runs at 19200 (about 150-190 ms a sector, with about twenty 9 s stalls and many edge aborts, not yet
explained). So the speed negotiation is entirely the Atari's driver: the OS ROM never speaks `3F`;
the U1MB BIOS, SpartaDOS X, MyPicoDOS's HISIO and the Happy/US Doubler/Ultra Speed drivers inject
it. An application without one of those stays at 19200 unless it embeds a driver (MyPicoDOS's HISIO
module is built for that; it is GPL, so check the licence before embedding it in nio-core-apps or
fujinet-nio-lib).

Second listener experiment (observation only, no behaviour change): the original ESP32 has UART0
(console), UART1 (SIO) and a free UART2. A `UartChannel` on UART2 shares the SIO RX pin through the
GPIO matrix, at the speed the main UART is not using, and counts the 5-byte command frames it decodes
with a good checksum. `sio.status` shows `second listener (UART2, same RX pin) at N baud: command
frames decoded N`. With the main UART at high speed it should decode the app's `0x7f` frames at
19200; if it does, the dual-listen idea is feasible. TX stays on UART1.

Seventh result (bad SSID, U1MB driver off, so 19200 and no HSIO anywhere): the 9 s stalls are still
there, so Wi-Fi is not the cause (and `aborts: edge` is 1, so it is not CMD noise either). A stall is
8.99 s of bus silence, a PAL SIO timeout (7 x 64 frames): the Atari sent a command, our reply did not
complete on its side, and it re-sent after the timeout (121 disk frames for 112 sector reads). Likely
cause: at 19200 we sent ACK, COMPLETE and the data back to back, where the legacy firmware waits
250 us after the ACK (`DELAY_T5`) at every speed; the Atari's ROM routine sometimes misses COMPLETE.
The 250 us ACK gap now applies at standard speed too (the 1 ms COMPLETE-to-data gap stays high-speed
only). Same cause could be behind the long first Query in 007. To confirm: no 9 s stalls at 19200.

Eighth result (second listener, index 1): UART2 on the shared RX pin decoded the app's first `0x7f` Query
at 19200 while the main UART was at 108,800 and failing, so the pin sharing works. Sector time at index 1
is about 33 ms (19200 with the U1MB driver on and no index set: about 200 ms; U1MB driver off: about 150 ms).
Now built: when the second listener decodes a good command frame for the command that just ended, the
capture drops the main UART's reading of it, the ingress switches the main UART to that speed
(TX follows: it is the same UART) and the frame is handled at once, so no command is lost and no retries
are needed. The two-bad-frames toggle stays as the backup. `sio.status` shows how many frames were used
this way.

Ninth result (probe and benchmark, index 1, U1MB `All`): the U1MB never sends `3F` to `0x71` or `0x7f`;
it sends their frames at 19200 (the first eight other-device frames are all `0x71 0x53`, no probe).
A native read of 256 bytes costs 246 ms (1 KB/s, 3 frames per read) at 19200, the baseline. So the
Atari side has to choose the speed for the native endpoint: `fujinet-nio-lib` now has an opt-in driver,
`fn_atari_hsio_enable()` (a port of Matthias Reichl's HISIO, GPL 2+, in `src/platform/atari/fn_hsio.s`),
which sends `3F` to `0x7f` and then runs every native call at the answered index. ftestnio's H key
toggles it. The firmware needs nothing new for this: `3F` on `0x7f`, the per-frame speed following and
the reply gaps are all in place. The U1MB author (flashjazzcat) has been asked why non-disk devices are
not sped up. The U1MB manual says `SIO devices: All` covers every device except cassette and the OS
"New Poll"; this is not what is observed.

Tenth result (the library driver, index 1): a native read went from 246 ms (19200) to 94 ms, then 87 ms
after the loop stopped sleeping a tick while a write waits for its data. The driver counted 5 errors in
100 reads, all timeouts (`$8A`): no framing, overrun, checksum or NAK, so the receive loop and the baud are
fine. Timeouts at the ACK retry at the same speed (about 40 ms each); the standard-speed fallback is now
rare (3 rescues against 9). About one CMD edge per frame aborts the capture (`aborts: edge` is about the
frame count at every speed): an edge during a write's data phase kills the write, so the Atari gets no
ACK for its data frame. Edges still pending when a reply ends with CMD high are now dropped as noise from
our own transmission (the Atari does not touch CMD then); `sio.status` counts them as `CMD edges while we
transmitted, dropped as noise`. To confirm: fewer timeouts and `aborts: edge` falling.

First hardware result (U1MB, index 6): the Atari asked `3F` once and we switched, but two command
frames were unreadable and the fallback returned us to 19200. The Atari never asks again and keeps
retrying at high speed (about 14 times per command, then once at standard), so every sector took
about 0.68 s. The fallback assumes a fresh `3F` after a reset, which this driver does not send.
`sio.hsio 6 nofallback` and the bad-frame record in `sio.status` are there to find out why the
first frames were unreadable.

1. Measure on hardware with the U1MB: `sio.hsio 6`, boot, then `sio.status` (look at switches,
   fallbacks, `uart_rx_errors`, `react`/`reply`), then try 3 and 0.
2. A persistent setting: yaml, the native endpoint with a Python client and `fujinet-nio-lib` call.
