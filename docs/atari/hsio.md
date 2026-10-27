# High-speed SIO (HSIO) on the Atari

Status: not implemented. `3F` is NAKed, so the Atari stays at 19200 baud. This records how the
legacy firmware does it, what was checked, and the plan. Measured numbers are in
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

## Plan

1. **Measure (done):** `sio.status` shows the SIO UART baud, and `sio.baud` (Atari idle) compares what
   the ESP32 UART actually produces for each index against the POKEY rate for PAL and NTSC clocks.
   TX pacing (`tx_chunk_gap_us`, meant for Amiga RS-232 overruns) is forced off for the SIO UART.
2. `3F` on `D1:`-`D8:`, with the baud switch and the 1 ms gap before data frames, default off.
3. The `$80` command variants, and the checksum-failure toggle.
4. A setting for the index (yaml, console, the native endpoint with a Python client and
   `fujinet-nio-lib` call), not a legacy `$70` device.
