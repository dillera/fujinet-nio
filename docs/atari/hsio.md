# High-speed SIO (HSIO) on the Atari

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

First hardware result (U1MB, index 6): the Atari asked `3F` once and we switched, but two command
frames were unreadable and the fallback returned us to 19200. The Atari never asks again and keeps
retrying at high speed (about 14 times per command, then once at standard), so every sector took
about 0.68 s. The fallback assumes a fresh `3F` after a reset, which this driver does not send.
`sio.hsio 6 nofallback` and the bad-frame record in `sio.status` are there to find out why the
first frames were unreadable.

1. Measure on hardware with the U1MB: `sio.hsio 6`, boot, then `sio.status` (look at switches,
   fallbacks, `uart_rx_errors`, `react`/`reply`), then try 3 and 0.
2. A persistent setting: yaml, the native endpoint with a Python client and `fujinet-nio-lib` call.
