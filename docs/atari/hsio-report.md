# High-speed SIO on the Atari: trials, results and the numbers we chose

This is the evidence behind the timings and settings in the Atari SIO ingress
(`src/platform/esp32/atari_sio_ingress.cpp`, `include/fujinet/io/atari_sio_*.h`). The design and the
running notes are in [hsio.md](hsio.md); this page says, for each value, what was tried and what
decided it. Where a value is a guess or a copy from the legacy firmware, it says so.

## Test setup

- FujiNet: classic ESP32 (WROVER-E), board `atari-nio-fujibus-sio-gpio-fujinet-v1-16mb`. SIO UART1
  (RX GPIO33, TX GPIO21), CMD on GPIO39 (ISR), UART2 as a second listener on the RX pin, UART0 console.
- Atari: PAL 800XL with a U1MB, SIO driver HSIO, SIO devices `All`.
- Software: the boot disk starts `ftestnio` (nio-core-apps). Its keys: **B** reads 100 sectors through
  the native endpoint and times them; **T** times 100 calls each of six call types; **H** turns the
  library's own high-speed driver on and off; **P** sends one raw command to each of several device IDs.
- Firmware diagnostics: `sio.hsio <0-9>` sets the speed index, `sio.status` shows what the SIO task saw,
  `sio.reset` zeroes the counters.
- Timing: `OS.rtclok` jiffies (20 ms on PAL), over 100 calls, so one figure is good to about 0.2 ms.

Speeds: POKEY divisor `n` gives PAL clock / (2 x (n + 7)). The `3F` reply is the divisor; the legacy
firmware turns it into a baud as `1773447 x 10 / (10 x 2 x (index + 7) + 3)`: index 1 = 108,800 baud,
index 6 = 67,431, index 0 = 124,017. Index 10 (`$0A`) is read by drivers as a Happy 1050, so only 0-9 are
offered.

## What the trials found, in order

1. **Index 6, first try.** The Atari asked `3F` once, we switched, two command frames were unreadable
   and our fallback returned us to 19200. The Atari never asks again and kept retrying at high speed, so
   every sector took about 0.68 s. Lesson: the fallback must be able to go back *up*, because this
   driver only asks at the start.
2. **Toggle in both directions**, as the legacy firmware does (it flips the baud after two bad command
   frames, either way). It worked but flapped (62 switches in 180 sectors) and sectors took either 50 ms
   or 200-300 ms with bursts of UART breaks.
3. **Reply gaps added** (250 us after ACK, 1 ms between COMPLETE and data at high speed). The boot
   became steady: 112 sectors at about 50 ms with one switch.
4. **The app's native commands went at 19200.** The U1MB probes only `D1:` with `3F`; `0x70`, `0x71`
   and `0x7f` frames were sent at 19200 with no probe (the first eight frames for other devices were all
   `0x71 0x53`). Answering `3F` on `0x7f` changed nothing. With the U1MB driver off nothing probes at
   all, so the speed negotiation is entirely the Atari driver's job: the OS ROM never speaks `3F`.
   The U1MB manual says `All` covers every device except cassette and the OS "New Poll" device; that is
   not what we see, and the U1MB author has been asked.
5. **9 s stalls at 19200** (about twenty in a 112-sector boot, with the U1MB driver off, Wi-Fi ruled out
   by a bad-SSID run). 8.99 s is exactly a PAL SIO timeout (7 x 64 frames): the reply did not complete
   on the Atari's side and it re-sent after the timeout. Sending ACK, COMPLETE and the data back to back
   at 19200 was the cause; the 250 us gap after ACK at every speed removed the stalls.
6. **Second listener.** UART2 on the shared RX pin, at the other speed, decoded the app's `0x7f` frames
   while UART1 was failing on them. Frames it decodes with a good checksum are dispatched at once and
   the main UART is switched to that speed, so a speed change costs no command (previously two were
   lost each time, and each loss was an ACK timeout and retry). Boot at index 1: about 33 ms a sector.
7. **A library driver for native calls.** `fn_atari_hsio_enable()` in `fujinet-nio-lib` is a port of
   Matthias Reichl's HISIO (GPL 2 or later, credited in the file and `THIRD_PARTY.md`). It sends `3F` to
   `0x7f` and runs every native call at the answered index. A native read of a sector went from 246 ms
   (19200) to 87-94 ms.
8. **Dropping CMD edges seen while we transmit.** About one CMD edge per frame aborted the capture
   (`aborts: edge` close to the frame count at every speed), and I suspected these killed a write's data
   phase and caused the ACK timeouts. Dropping edges still pending when a reply ends with CMD high removed
   158 edges at 19200 and 33 at high speed, and made no difference: timeouts stayed at 3-5 in 100 reads
   (final run: 18 in about 600 native calls). **Conclusion: those edges are not the cause of the
   timeouts.** The dropping is harmless and stays; the cause of the remaining timeouts is not found.

## Per-call timing (the T page)

100 calls each, index 1, boot disk with 256-byte sectors. Times are ms per call. "OS" rows go through
the OS SIO entry (and so the U1MB driver); "native" rows are the library's FujiBus calls, which are
three SIO transactions each (frames: 1404 high-speed, 1570 standard, across 600 timed calls).

| Call | Standard (19200) | High speed (index 1) |
|---|---|---|
| OS `D1:` status, 4 bytes | 6.2 | 6.2 |
| OS `0x7f` query, 4 bytes | 15.4 | 15.2 |
| native info (tiny payload) | 91.2 | 34.6 |
| native read, 128 bytes | 156.2 | 61.4 |
| native read, 256 bytes | 225.8 | 81.8 |
| native read, 512 bytes (disk sector is 256) | 228.4 | 82.4 |

Library errors in the high-speed run: 18 timeouts, no NAK, framing, overrun or checksum errors.

What this says:

- **The raw `0x7f` query costs the same in both runs (15.4 and 15.2 ms)**: the U1MB sends it at 19200
  either way, so that row is the standard-speed single-frame cost and is not a high-speed figure. `D1:`
  at 6.2 ms (U1MB at high speed) is the floor for one small transaction with this setup.
- **Fixed cost per native call**: 91.2 ms at 19200 and 34.6 ms at high speed, so about 30 and 11.5 ms for
  each of the three transactions. That is 2.6 times faster, not the 5.7 times the baud ratio suggests,
  so about 6 ms per transaction is not wire time. `D1:` status at 6.2 ms is about the same floor.
- **Data cost**: 128 more bytes cost 69.6 ms at 19200 (wire time for 128 bytes is 66.7 ms, so it is
  wire-bound) but 20.4 ms at high speed (wire time 11.8 ms). At high speed we get about 6.3 KB/s of a
  possible 10.9 KB/s. **The 8.6 ms of extra per 128 bytes is unexplained.** Candidates: the ESP's TX
  chunking, the Atari receive loop's per-byte cost, or something in the reply path. The 512-byte row
  being no slower than 256 shows the limit is the sector, not the call.
- **Timeouts cost about 3 percent.** 18 timeouts at about 40 ms each is 0.7 s of a 26 s run.
- **Rule of thumb for the 256-byte read**: 81.8 ms against 225.8, so 2.8 times faster; 3.1 KB/s against
  1.1 KB/s. Asymptotically (large reads, if the protocol allows them) about 6.3 KB/s against 1.9 KB/s.
- **The remaining lever is the protocol**, not the baud: each read is three transactions, and the fixed
  cost of about 11.5 ms each at high speed is nearly all of a small read.

## The numbers we use, and why

| Value | Where | Basis | How solid |
|---|---|---|---|
| ACK gap 250 us, at every speed | `ack_gap_us` | The legacy firmware's `DELAY_T5`. Trial 5: without it at 19200, about twenty 9 s stalls in a 112-sector boot; with it, none. | Tested for the effect (and at high speed: 0 is unusable, 346 fallbacks); the 250 us size is the legacy value, not tuned. |
| COMPLETE-to-data gap 1 ms, high speed only | `hsio_data_gap_us` | The legacy `DELAY_HSIO_DATA`, kept for SpartaDOS X's driver, which misses a data frame that follows COMPLETE at once. Added together with the ACK gap in trial 3, when the boot went from flapping to steady. | Isolated once: 0 saves about 1 ms per reply with data (4 percent of a read) and broke nothing, but timeouts and fallbacks were higher in three single runs. Kept until repeated runs show it is free. |
| Speed index 1 (108,800 baud) as the suggested value | `sio.hsio 1` | Sector times: index 6 about 50 ms, index 1 about 33 ms, index 0 about 30 ms. Index 0 is 124,017 baud, which the Atari-side receive loop only just meets (it crosses a page in our port, which costs cycles), for 3 ms more. | Measured on disk boot only; index 0 not tested with native calls. |
| Fall back to 19200 after 2 bad command frames, and back up after 2 more | `AtariSioCapture` | The legacy `COMMAND_FRAME_SPEED_CHANGE_THRESHOLD`. Trial 1 shows a one-way fallback strands the Atari at high speed; trial 2 shows the toggle follows it. | The threshold is the legacy value. The second listener is now the main mechanism and this is the backup (final run: 4 fallbacks, 11 frames decoded by the second listener, 8 of them used). |
| A frame decoded by the second listener is valid for 10 ms | `atari_sio_ingress.cpp` | A heuristic: it must outlast one poll of the SIO task (the longest idle poll gap measured is about 10 ms) and be much shorter than the Atari's retry interval. | Varied (5, 10, 20 ms): no measurable difference. Kept at 10. |
| Command ACK within 16 ms of CMD release | `AtariSioReplyStats::ack_deadline_us` | The SIO timing limit (T2 maximum) for the device's ACK. | A specification value. Measured worst case is 13.4-14.9 ms, which is too close; typical is 1.1 ms. The worst cases happen when the console prints `sio.status` and blocks the SIO task, so they are a measurement artefact for now, but the margin is real. |
| Device whitelist (`0x31-0x38`, `0x7f`) and a checksum for second-listener frames | `AtariSioIngress` | A 5-byte window slides over arbitrary bytes at the other speed, so a frame must look like one for a device we serve. | By construction. |
| Short frames (1-4 bytes at CMD release) count as bad | capture | A high-speed frame read by a 19200 UART arrives as one or two garbage bytes (`0xe0 ...`), never five. | Observed in every run. |
| Drop CMD edges pending at end of a reply when CMD is high | `drop_tx_noise_edges` | Trial 8. | **Did not fix what it was for.** Harmless; kept because the Atari never touches CMD during our reply. |

## The FujiNet patch of Hias's driver (mozzwald, `highspeed-sio` branch `fujinet`)

The legacy firmware team's patched HISIO (compared against Hias's `master`). It explains the device
coverage we saw; it does not touch the timing.

- **Stock HISIO speeds only disks.** The original detection code is `LDA DDEVIC; CMP #$31; BNE STDSIO`:
  anything that is not device `$31` goes at standard speed, and the speed table is indexed by `DUNIT`.
  A driver derived from it that kept this would send `0x70`, `0x71` and `0x7f` at 19200 without a `3F`,
  which is what the U1MB does. This is a second explanation for trial 4, next to the U1MB's own settings;
  I have not seen the U1MB's source, so it is an inference.
- **The patch widens that to `$31-$38` and `$70-$78`** (build option `FUJINET`). It works on the
  effective device ID, `DDEVIC + DUNIT - 1`, and probes each with `3F` the way a disk is probed. Speeds
  are cached per effective ID in a 4-entry rotating table (`DEVICETB`/`SPEEDTB`) instead of per `DUNIT`.
- **`$7F` is outside that range.** Even this patched driver would not speed up our native endpoint. The
  library driver is still needed, or the native endpoint would have to answer on an ID in `$70-$78`.
- **No timing changes.** The engine, retries, ACK wait and receive loop are as in Hias's code; the diff
  adds no gaps and no timeouts. It cannot explain the ACK timeouts or the per-byte cost.
- **It confirms how the legacy firmware's `3F` behaves.** The chain-boot prototype sends one `3F` to
  `D1:` "to force FujiNet's disk baud toggle back to standard speed" and then clears the cache so the
  next boot probes again, so the legacy firmware toggles its baud on `3F` as we found, and a driver that
  forgets its cache re-probes.
- **Other changes** are unrelated to timing: a FujiNet chain-boot (mount all slots with `$70 $D7` then
  boot D1:), the keyboard-IRQ patch switched off, and a fix to a one-byte timer comment.

What it changes for us: (1) for NOS, whose network devices are `$71`-`$78`, the answer on the firmware
side is whether nio should serve `3F` on those IDs, as the legacy firmware does, so stock-patched drivers
speed them up; (2) our `0x7f` still needs the library driver (or a patch that includes `$7F`).

## Dial trials (index 1, high speed, T page, 100 calls per row)

Same setup as above. Each setting was run once; the counters and the T page were reset between runs.

| Setting | D1: status | lib Q | lib P | info | read 256 | `hs errors` timeouts | switches | edge aborts |
|---|---|---|---|---|---|---|---|---|
| defaults (ack 250, data 1000, window 10) | 6.0 | 6.0 | 6.4 | 34.6 | 82.2 | 26 | 4 | 182 |
| data gap 0 | 5.4 | 5.0 | 4.6 | 33.6 | 79.0 | 44 | 9 | 383 |
| data 0, window 5 | 5.0 | 5.0 | 4.6 | 34.2 | 79.4 | 17 | 7 | 228 |
| data 0, window 20 | 5.0 | 5.2 | 4.6 | 33.4 | 80.8 | 40 | 8 | 482 |
| ack gap 0 (and data 0) | 15.8 | 134 | 171 | the application hung | | | 346 | 1099 |

- **Single high-speed transactions are cheap**: a `Q` or `P` to `0x7f` through the library driver takes
  4.6-6.4 ms, against 15.2 ms for the same `Q` at 19200 through the OS. So the ESP and the Atari driver can
  turn a frame round in about 5 ms; a native call's 34 ms for three transactions is more than three times that.
- **ESP data frames go out at the wire rate**: 92.0 us/byte on average against 91.9 for 108,800 baud, 275
  bytes in 25.3 ms for a 256-byte sector (the FujiBus header and checksum are 19 bytes more than the sector).
  The excess per byte is not in our UART write.
- **ACK gap 0 is unusable** at high speed too: 346 fallbacks, 243 overflows, and `0x7f` frames were
  refused after the stream went out of step. The 250 us ACK gap stays. A value between 0 and 250 would save
  at most that much per command and was not tried.
- **Data gap 0 saves about 1 ms per reply that carries data**: lib Q 6.0 to 5.0, lib P 6.4 to 4.6, about 3 ms
  of a 82 ms read (4 percent). It did not break anything, but timeouts and fallbacks were higher in all
  three runs (17-44 against 26; switches 7-9 against 4). One run each is too noisy to call that a
  difference, so the gap stays at 1 ms until repeated runs show it is free.
- **The second-listener window (5, 10, 20 ms) made no measurable difference** (read 256: 79.4, 82.2, 80.8
  with other settings varying too; timeouts 17, 26, 40). It stays at 10 ms: nothing says another value is
  better, and 5 and 20 are both inside the noise.
- **Timeouts vary a lot between identical runs** (17 to 44 in about 600 calls) and appear to follow the
  number of edge aborts (182 to 482; 17 and 26 at the low end, 40 and 44 at the high end). Four runs is
  suggestive only. If the abort is the cause, an abort while a write waits for its data would explain it:
  the Atari gets no ACK for its data frame and times out. The firmware now counts those separately, and
  `sio.tune glitch 1` ignores CMD edges seen with the line high while a write waits.

## Next trials

New measurements and one dial, all in `sio.status`:

- `per command (...)`: for each of `Q`, `W`, `P`, `R` the average time the FujiNet took from the CMD
  release to the end of its last reply byte (service), and then how long the Atari took until the CMD
  release of its next command (idle). Summed over a run these equal the elapsed time, so they show which
  side the remaining 40 ms of a read belongs to. If the idle time after `R` is large, it is the Atari's
  decoding of the response; if the service time of `W` is large, it is the FujiNet.
- `edge aborts while a write waited for its data` and `CMD edges ignored there`.
- `sio.tune glitch 1`: ignore CMD edges seen with the line high while a write waits for its data. Run
  **B** or **T** three times with it off and three times on, `sio.reset` before each, and compare
  `hs errors to`, `switches` and the read times.

## Open items

- **The ACK timeouts** (about 3 percent of native transactions, each retried at the same speed for
  about 40 ms): cause unknown. Not CMD edges while we transmit (trial 8).
- **Per-byte cost at high speed** is 1.7 times the wire time.
- **The U1MB speeds up only `D1:`**: awaiting an answer from its author. The library driver covers the
  native endpoint meanwhile.
- **Reply timing fix.** A stray CMD edge stamped just after the reply began made `reply max` wrap to
  4,294,967 ms in the last run (`over 16 ms: 1`). It now counts as 0.
- **Console output** from `sio.status` stalls the SIO task for about 100 ms (the `poll gap` watch); the
  figure is an artefact of the diagnostics, not the bus.
- **Not done yet:** a persistent HSIO setting (yaml, native-endpoint command, Python client, library
  call); emulators (AltirraSDL/NetSIO) answering `3F`.
