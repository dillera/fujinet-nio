# NetworkDevice Binary Protocol

This document specifies the **binary, non-JSON** request/response format used by the **NetworkDevice** (`WireDeviceId::NetworkService`, proposed `0xFD`).

Design goals:

- **8-bit-host friendly** (no JSON, minimal parsing)
- **Transport agnostic** (FujiBus+SLIP today; compatible with future transports)
- **Streaming and chunkable** (arbitrary response sizes via explicit offsets)
- **Poll-driven** (device progresses network work in `poll()`; host pulls results)
- **Explicit state** (network operations are represented by handles)

This protocol sits **above** any HTTP/TLS implementation and **above** any socket stack. It is purely a device protocol.

This document describes the v1 NetworkDevice binary protocol and its command
semantics (Open / Read / Write / Info / Close).

The protocol is transport-agnostic: multiple network schemes may be implemented
over it. At present, two schemes exist:

- HTTP/HTTPS (request–response semantics)
- TCP (stream socket semantics)

HTTP behavior is documented inline below.
TCP stream behavior is documented separately in:

  docs/network_device_tcp.md

Unless explicitly stated otherwise, command semantics described here apply to
both HTTP and TCP. Where semantics differ (e.g. offsets, EOF behavior, Info()),
the TCP document is authoritative.

---

## Terminology

- **Host**: remote client sending requests (Python tooling, emulator, 8-bit machine).
- **Device**: `NetworkDevice` in fujinet-nio.
- **Handle**: `u16` session identifier allocated by `Open` and released by `Close`.
- **LE**: little-endian.
- **Chunking**: host requests data in blocks using `offset` + `maxBytes`.

---

## Common Encoding Rules

### Byte order
All multi-byte numeric values are **little-endian**.

### Strings
Strings are **length-prefixed** and contain raw bytes (typically ASCII/UTF-8). They are **not null-terminated**.

- `u16 len` + `len bytes` for URLs, header keys/values, etc.

### Versioning
Every request payload begins with:

- `u8 version`

Current version: `1`

If the device receives an unknown version, it must respond with `StatusCode::InvalidRequest`.

---

## Transport Framing (Non-normative)

This protocol is agnostic to the underlying transport framing (e.g. SLIP, USB CDC, UART buffers).

- Hosts MUST NOT assume that a single Read or Write maps to a single transport frame.
- Devices MUST present protocol-correct responses independent of transport buffering.
- Any transport-imposed size limits are handled by host-driven chunking, not by the protocol.


---

## Status and Transport Convention

This protocol does **not** define how status is transported. In fujinet-nio today:

- `StatusCode` is carried by the **transport** as FujiBus response `param[0]`
- the NetworkDevice protocol data is carried in the **FujiBus packet payload**

This keeps the NetworkDevice protocol independent of FujiBus/FEP-004 details.

---

## Device and Command IDs

- Device: `WireDeviceId::NetworkService` (proposed `0xFD`)
- Commands: `NetworkCommand` enum (`uint8_t`) interpreted from the low 8 bits of `IORequest.command`

Command IDs (v1):

| Command | ID | Purpose |
|--------:|---:|---------|
| `Open`  | `0x01` | Create a request/session handle |
| `Read`  | `0x02` | Read response bytes at offset |
| `Write` | `0x03` | Write request body bytes at offset |
| `Close` | `0x04` | Release a handle |
| `Info`  | `0x05` | Fetch response metadata (HTTP status, headers, length) |

v1 goal: support streaming reads without buffering entire bodies in RAM.

---

## Common Response Prefix

Many commands return this prefix:

```
u8   version            // = 1
u8   flags              // command-specific
u16  reserved           // = 0 for now (LE)
```

Reserved is for forward-compatible expansion.

---

## Method Encoding

The `Open` command uses a compact method code:

| Method | Code |
|-------:|-----:|
| GET    | 1 |
| POST   | 2 |
| PUT    | 3 |
| DELETE | 4 |
| HEAD   | 5 |

---

## Command: Open (0x01)

Create a session handle for a URL. `Open` is allowed to be asynchronous: it may return immediately and the session becomes readable later (via `poll()`).

### Request

```
u8   version
u8   method            // 1..5
u8   flags             // see below
u16  urlLen            // LE
u8[] url               // length urlLen

u16  headerCount       // LE

repeat headerCount times:
  u16  keyLen          // LE
  u8[] key             // length keyLen
  u16  valLen          // LE
  u8[] value           // length valLen

u32  bodyLenHint       // LE; 0 if unknown

u16  respHeaderCount   // LE; number of response header names to capture

repeat respHeaderCount times:
  u16  nameLen         // LE
  u8[] name            // length nameLen (header name, ASCII; case-insensitive)

[optional if more bytes remain]
u32  openExtFlags      // LE; presence bits for extension blocks

if (openExtFlags bit0 set):
  u8   translationType   // 0=None, 1=Json, 2=Xml, 3=Rss, 4=Image
  u8   translationFlags  // translator-specific, 0 for now
  u16  selectorLen       // LE; 0 = no selector
  u8[] selector          // length selectorLen

if (openExtFlags bit1 set):
  u8   contentProfile    // 0=None, 1=JsonBody, 2=FormBody, 3=TextBody
```

### Open flags (u8)
bit0 = tls
bit1 = follow_redirects
bit2 = body_unknown_len  // POST/PUT only, bodyLenHint==0
bit3 = allow_evict
bit4 = stream_no_probe   // streaming-only hint: host will frame reads itself
bit5 = keep              // keep until closed (see below)

keep semantics (bit5):
- A session with no request for the idle timeout is closed by the device, so a client that went away (an Atari
  has no reset line the firmware can see) does not hold a handle for ever. The timeout is wall-clock time
  (default 20 minutes; a POST/PUT still waiting for its body: 10 seconds), measured from the last request on
  that handle, not poll ticks.
- A session opened with `keep` is exempt: it stays until the host sends `Close`, or the client's host reset
  reclaims it. Use it for a long-lived connection that may be quiet for a long time (a pipe read on demand).
- A `keep` session is never chosen for eviction, whatever `allow_evict` says: if every handle is a `keep`
  session, `Open` returns `DeviceBusy`.
- Firmware that predates the flag ignores the bit, so a client may always set it; it just keeps the old timeout.

allow_evict semantics:
- When allow_evict=0 (default), `Open` MUST return `DeviceBusy` if no free handles are available.
  The device MUST NOT evict an existing active handle.
- When allow_evict=1, the device MAY evict the least-recently-used (LRU) active handle that is not a `keep` session to satisfy the Open.
  Any evicted handle becomes immediately invalid; subsequent Info/Read/Write/Close on it MUST return InvalidRequest.

### Response

```
u8   version
u8   flags               // bit0=accepted, bit1=needs_body_write
u16  reserved            // = 0
u16  handle              // LE
u8   proto_flags         // protocol capability flags
```

### Protocol capability flags (proto_flags)

The `proto_flags` byte informs the host about the protocol's behavioral requirements:

| Bit | Name | Meaning |
|----:|------|---------|
| 0 | `sequential_read` | Reads must use sequential offsets (streaming protocols) |
| 1 | `sequential_write` | Writes must use sequential offsets (streaming protocols) |
| 2 | `streaming` | Protocol is streaming (TCP, TLS) vs request/response (HTTP) |

Protocol examples:
- **HTTP/HTTPS**: `proto_flags = 0x00` (non-streaming, random-access reads)
- **TCP/TLS**: `proto_flags = 0x07` (streaming, sequential read/write)

Hosts MUST use these flags to determine offset behavior rather than detecting URL schemes.

### Status codes

- `Ok`: handle allocated (even if network work is not complete yet)
- `DeviceBusy`: no free handles
- `NotReady`: network unavailable
- `InvalidRequest`: malformed payload
- `InternalError`: unexpected failure

Notes:
- `needs_body_write=1` indicates the host must stream body via `Write` (POST/PUT).
- `accepted=1` means the handle exists; it does not imply the request has completed.

### Content Translation (v1)

Open can optionally configure a translated response view. The transport backend still
fetches raw bytes; the device applies translation before exposing data via `Read`.

To preserve compatibility with older clients, the extension area is optional. If the
Open payload ends immediately after the response-header list, the device behaves as if
`openExtFlags == 0` and no translation is requested.

Wire fields:

```
u32  openExtFlags

if openExtFlags & 0x00000001:
  u8   translationType
  u8   translationFlags
  lp_u16 selector
```

Defined open extension flags:
- bit 0 = translation block present
- bit 1 = request content profile block present

Defined content profiles:
- `0` = `None`
- `1` = `JsonBody` → `Content-Type: application/json`
- `2` = `FormBody` → `Content-Type: application/x-www-form-urlencoded`
- `3` = `TextBody` → `Content-Type: text/plain`

When a content profile is set, the device injects the corresponding request
header before dispatching the HTTP request. Explicit headers supplied in the
Open request take precedence; the profile is ignored for any header name the
client already provided (case-insensitive match).

Defined types:
- `0` = `None`
- `1` = `Json`
- `2` = `Xml` (reserved, currently unsupported)
- `3` = `Rss` (reserved, currently unsupported)
- `4` = `Image` (PNG/JPEG/GIF in, an indexed image out; see [Image translation](#image-translation-type-4))

For `Json`, `selector` is a JSON Pointer (RFC 6901), for example `/url`.

#### Image translation (type 4)

Type 4 decodes a PNG, JPEG or GIF response body into an indexed image and
returns it in the format the selector names. It works in two stages:

1. A format-neutral pipeline decodes the body, scales it to fit the output
   box, builds a palette, and maps every pixel to a palette index (nearest
   colour, optionally Floyd-Steinberg dithered).
2. The writer for `fmt` turns that indexed image into the bytes `Read`
   returns. `ilbm` is the only writer today.

The selector is ASCII `key=value` pairs separated by `,`. Every key is
optional and may appear at most once. An unknown key or value is an error:
`Open` or `TranslateConfigure` returns `InvalidRequest`.

| key | values | default | meaning |
|---|---|---|---|
| `fmt` | `ilbm` | `ilbm` | output format (the writer) |
| `w` | 16..1024 | 640 | max output width in pixels |
| `h` | 16..1024 | 400 | max output height in pixels |
| `colors` | 2..the format's pens (32 for `ilbm`) | 16 | number of image colours |
| `base` | 0 and up, if the format has pens (`ilbm` does) | 0 | first pen index; `base+colors` must fit the format's pens |
| `bits` | 1..8 | the format's; 4 for `ilbm` | bits per RGB channel in the palette: each channel is snapped to 2^bits evenly spaced levels, 0 and 255 included. `8` is full 24-bit colour |
| `par` | `X:Y`, 1..4 each | `1:1` | display pixel aspect (width:height); hires non-laced is `1:2` |
| `dither` | `fs` \| `none` | `fs` | Floyd-Steinberg error diffusion or nearest colour |
| `mode` | `auto` \| `gray` \| `color` | `auto` | `auto` = gray if every pixel has max(r,g,b)-min(r,g,b) <= 24 |
| `up` | `0` \| `1` | `0` | allow enlarging images smaller than the box |

`colors` and `base` describe pens of an indexed display. Each format sets
how many pens it holds (`ilbm`: 32, five bitplanes), its default `bits`, and
whether `base` applies; the selector is checked against them once every key
is read, so `fmt` may appear anywhere. The format table is
`src/lib/image/output_format.cpp`.

Palette:
- Gray: `colors` evenly spaced levels from black to white, each snapped to
  `bits`.
- Colour: median cut over a histogram with 4 bits per channel (4096 bins).
  Each box gives one entry, snapped to `bits`: the box's mean bin when
  `bits` is 4 or less, the mean of the box's actual pixels when it is more.
  Entries left over when the image has fewer distinct colours are black.

Pixels are mapped to the nearest palette entry by squared RGB distance
weighted 3:6:1.

##### `ilbm` writer

- A standard `FORM ILBM` containing BMHD, CMAP and BODY.
- BMHD values:
  - `w`/`h` are the output dimensions, `x=y=0`.
  - `nPlanes` is the bit count needed for `base+colors` pens, `masking=0`, `compression=1` (ByteRun1), `transparentColor=0`.
  - `xAspect`/`yAspect` come from `par`, `pageWidth=w`, `pageHeight=h`.
- CMAP has `3 << nPlanes` bytes of 8-bit RGB. Entries below `base` and at or
  above `base+colors` are written as 0. With the default `bits=4` every entry
  is an Amiga OCS (12-bit) colour, each channel a multiple of 17; `bits=8`
  suits AGA.
- BODY rows are `((w+15)/16)*2` bytes per plane. Each row and each plane is ByteRun1-compressed separately, in plane order 0..n-1.
- Chunks with an odd length get a pad byte, and the FORM length is correct.

A selector without `fmt` and `bits` gives the same bytes as firmware from
before those keys existed.

##### Limits and errors

- Pixel cap: a source image whose width*height is above the cap returns
  `Unsupported`. The cap is `translation.image.max_pixels` in `fujinet.yaml`:

  ```yaml
  translation:
    image:
      max_pixels: 0   # 0 = the platform default: 1048576 (1024x1024) on ESP32,
                      # 16777216 (4096x4096) on POSIX
  ```

  The console shows and changes it with `net.translation.get`,
  `net.translation.set image_max_pixels <n|default>` and
  `net.translation.save` (see [diagnostics](diagnostics.md)). A change
  applies to translations configured after it. The platform default is
  `platform::default_image_max_pixels()`
  (`include/fujinet/platform/image_translation.h`).
- No side may be wider or taller than 8192 pixels (`STBI_MAX_DIMENSIONS`).
  stb_image rejects such a header like any other it cannot read.
- An undecodable body, including one over 8192 pixels on a side, returns
  `InvalidRequest` from `Info` or `Read`. An allocation failure while
  decoding or converting returns `Unsupported`.

##### Conversion time

Conversion runs synchronously inside the first `Info`, `Read` or
`TranslateConfigure` that finds the whole body, so the device answers nothing
else until it finishes. Allow for it in the client's timeout for that
request.

It runs on a temporary 48 KB stack (`ImageContentTranslator::kConvertStackBytes`)
from `platform::run_with_large_stack()`: a task with its stack in PSRAM on
ESP32, a thread on POSIX. The core task's own stack stays small. If that
stack cannot be had (an ESP32 without PSRAM), the conversion returns
`Unsupported`.

`net.translation.stats` on the console shows, for each recent translation,
the time it blocked the device, the stack it used and how far free heap fell
(see [diagnostics](diagnostics.md)), in any build. Firmware built with
`FN_DEBUG` also logs each one under the `net` tag
(`translation type=4: 10851 bytes in 107 ms, status 0`) and the image
details under `image`; without `FN_DEBUG` every `FN_LOG*` call compiles away
(`include/fujinet/core/logging.h`). POSIX `*-debug` presets define it; on
ESP32 add `-DFN_DEBUG` to `build_flags` in `platformio.local.ini`.

Measured, ESP32-S3 N16R8 (`net.translation.stats`), xkcd #2636: a 740x1215
8-bit gray PNG, 223158 bytes:

| selector | output | time | stack used | heap peak |
|---|---|---|---|---|
| `w=624,h=150,colors=12,base=4,par=1:2` | 182x149 | 954 ms | 9300 B | 3.7 MB |
| `w=640,h=512,colors=16` | 311x511 | 2551 ms | 9300 B | 3.7 MB |
| (over the old 490000-pixel cap) | rejected | 5 ms | 2376 B | 67 KB |

Free heap before was 8.2 MB, with a 7.6 MB largest block. Heap peak is about
4 bytes per source pixel for gray or palette PNGs (about 6 for RGB, 8 for
RGBA), which sets the ESP32 default cap at 1024x1024. The time beyond the
decode grows with the output size; those runs predate the faster palette
lookup (`NearestColour`), which cut that part by more than half on the host.

Stack used on POSIX (64-bit, debug / release) for the 16x12 test images:
PNG 14.8 / 16.0 KB, JPEG 17.6 / 7.4 KB, GIF 41.8 / 41.4 KB. GIF is why the
stack is 48 KB.

##### Compatibility

See [extending commands](protocol_reference.md#extending-commands). Image
translation adds a value to the existing `translationType` field and words to
the existing selector string, so no request or reply layout changed and
rules 1-4 there are unaffected. The selector words are the compatibility
surface instead:

- Firmware that predates the Image translator answers an `Open` (with the
  translation extension) or a `TranslateConfigure` carrying translation
  type `4` with `InvalidRequest`. A client should treat that as "image
  translation is not available on this firmware" and tell the user to update
  the firmware. (After the open succeeds, `InvalidRequest` from `Read` or
  `Info` means the body could not be decoded, as described above.)
- Firmware rejects selector keys and values it does not know, so firmware
  that predates a key (such as `fmt` or `bits`) or a format answers
  `InvalidRequest` at `Open`, before any body is read. For the widest
  compatibility, omit every key whose value is the default; `fmt=ilbm` and
  `bits=4` are never needed. Send a key only for a non-default value, and
  read `InvalidRequest` at `Open` as "this firmware lacks that option".
- New formats arrive as new `fmt` values with their own writer and default
  `bits`, so existing selectors keep their meaning.
- Older clients are unaffected: they never send type `4`.

When translation is active:
1. The device buffers the full HTTP response body.
2. It applies the selected translator to the cached body.
3. `Read` returns translated bytes instead of raw response bytes.
4. `Info` reports translated output size as `contentLength` while preserving transport metadata.

If `translationType == None`, the device behaves as before and exposes the raw response body.

When translation is active but the full body is not yet available, `Read` and `Info` return `NotReady`.

JSON and Image translation are implemented. A translation type the firmware
does not know (such as `4` on firmware without the Image translator) returns
`InvalidRequest` from `Open` or `TranslateConfigure`. `Xml` and `Rss` are known
but have no translator yet, and return `Unsupported`.

JSON serialization format (intended to be simple for 8-bit hosts):
- **String**: raw text content (no surrounding quotes)
- **Number**: integer or floating-point text
- **Boolean**: `TRUE` or `FALSE`
- **Null**: `NULL`
- **Object**: each `key\nvalue\n` pair sequentially
- **Array**: each element on its own line

This allows the 8-bit host to offload JSON parsing to the FujiNet device, reading only the extracted value(s).

### Response header capture (v1)

- The client provides a list of response header **names** in `respHeaderCount`.
- The device/backend stores **only** those headers (case-insensitive match).
- If `respHeaderCount == 0`, the device stores **no** response headers.
- Devices MUST NOT add implicit HTTP headers (e.g. Content-Type) unless explicitly requested by the host via headers in the Open request.
- This mechanism is the sole v1 mechanism for HTTP response header selection.

---

## Command: Write (0x03)

Stream request body bytes into an open handle (for POST/PUT) **or** stream bytes into a non-HTTP protocol (e.g. TCP).
Chunking is host-driven.

### Request
```
u8 version
u16 handle        // LE
u32 offset        // LE
u16 dataLen       // LE
u8[] data         // length dataLen
```

### Response
```
u8 version
u8 flags          // reserved; 0 for now
u16 reserved      // = 0
u16 handle        // LE
u32 offsetEcho    // LE
u16 writtenLen    // LE
```

### Status codes
- `Ok`
- `InvalidRequest` (bad handle, missing bytes, etc.)
- `IOError` (write failed)
- `DeviceBusy` (backpressure / buffer full; host should retry)

### Offset semantics (important)

The meaning of `offset` depends on the protocol bound to the handle:

- **HTTP request bodies (http/https)**:
  - Offsets MUST be sequential (0..N) and MUST NOT exceed `bodyLenHint`.
  - See `## HTTP request lifecycle (HTTP/HTTPS schemes)` for the authoritative commit rule.

- **Stream protocols (tcp)**:
  - Offsets represent a **monotonic write cursor** (bytes accepted so far).
  - The device/backend MUST require strictly sequential offsets:
    - If `offset` is a rewind or gap, return `InvalidRequest`.
  - Partial writes are allowed; `writtenLen` may be less than `dataLen`.
  - If the stream is not yet connected, the backend may return `NotReady`.

### TCP half-close (optional)

For TCP handles, a zero-length write MAY be used as a send-finish signal:

- `Write(handle, offset=write_cursor, dataLen=0)` MAY cause the backend to call `shutdown(SHUT_WR)`
  (if enabled by backend configuration such as `halfclose=1` in the URL).

### Offset semantics (scheme-dependent)

As with Read, the meaning of `offset` depends on the scheme:

- For HTTP/HTTPS:
  - Writes append to the outgoing request body.
  - Offset is typically ignored or must be zero.

- For TCP:
  - `offset` is a sequential stream cursor.
  - Writes MUST be strictly sequential.
  - A Write request whose `offset` does not match the current stream write
    cursor MUST fail with StatusCode::InvalidRequest, except that an exact
    duplicate of the immediately previous successful write MAY be acknowledged
    again without sending duplicate bytes to the TCP peer.

### Zero-length write (TCP half-close)

For the TCP scheme, a Write request with `length == 0` MAY be used to signal
end-of-stream (half-close), if enabled by the URL options. This maps to a
shutdown of the socket write side (e.g. `shutdown(SHUT_WR)`).

See docs/network_device_tcp.md for details.

---

## Command: Read (0x02)

Read response body bytes at an explicit offset. Chunking is host-driven.

### Request
```
u8 version
u16 handle     // LE
u32 offset     // LE
u16 maxBytes   // LE; requested bytes to read
```

### Response
```
u8 version
u8 flags       // bit0=eof, bit1=truncated, bit2=more_available, bit3=available_valid
u16 available  // LE; bytes the device still holds for this handle AFTER this read, when
               // flags bit3 is set (saturating at 65535); 0 and bit3 clear when the protocol cannot say
u16 handle     // LE
u32 offsetEcho // LE
u16 dataLen    // LE; actual bytes returned
u8[] data      // length dataLen
```

`available` was a reserved field that firmware always sent as 0, so a client that ignores it is unaffected and a
client that reads it checks bit3 first (an older firmware leaves it clear). It is a lower bound: more may arrive,
and TLS counts only bytes already decrypted. `more_available` (bit2) is the same fact as `available > 0` for
protocols that report it. A client reading a length-prefixed or request/reply stream can use it to know whether
the next read will find data already waiting, without a read that answers `NotReady`.

### Status codes
- `Ok` (possibly `dataLen=0`)
- `NotReady` (response not ready yet; host should retry later)
- `InvalidRequest` (bad handle or malformed payload)
- `IOError` (read failed)

Notes:
- `eof=1` indicates end-of-response reached.
- `truncated`: set when the device filled the requested max_bytes (i.e. caller buffer limit hit).
  It does NOT mean "server truncated the response".
- `more_available=1` indicates additional bytes are already immediately readable for this
  handle after the returned payload. It does not imply the peer may send more later; it is
  strictly a local availability hint for avoiding an extra probe read.
- The host reads until `eof=1` or `dataLen=0`.
- `offsetEcho` MUST equal the `offset` value from the corresponding Read request.
  Hosts MUST treat a mismatch as a protocol error.


### Offset semantics (important)

The meaning of `offset` depends on the protocol bound to the handle:

- **HTTP response bodies (http/https)**:
  - Offset represents the byte position within the response body stream as returned to the host.
  - The host MUST request subsequent chunks using increasing offsets.

- **Stream protocols (tcp)**:
  - Offset represents a **monotonic read cursor** (bytes delivered so far).
  - The device/backend MUST require strictly sequential offsets:
    - If `offset` is a rewind or gap, return `InvalidRequest`.

### Offset semantics (scheme-dependent)

The meaning of the `offset` field depends on the active network scheme:

- For HTTP/HTTPS:
  - `offset` refers to a byte offset within the response body.
  - Random-access reads MAY be supported by the backend (e.g. via range
    requests), but are not guaranteed.

- For TCP:
  - `offset` is a sequential stream cursor.
  - Reads MUST be strictly sequential.
  - A Read request whose `offset` does not match the current stream read
    cursor MUST fail with StatusCode::InvalidRequest, except that an exact
    duplicate of the immediately previous successful read MAY replay the same
    response bytes and flags without consuming more TCP bytes.

See docs/network_device_tcp.md for full TCP stream semantics.

#### READ behavior notes (important)

Backends may be synchronous (POSIX curl) or streaming/asynchronous (ESP32, TCP).

- If data is not yet available, READ may return `NotReady`.
- A READ returning `Ok` with `read_len == 0` is only valid when `eof == true` (transfer complete / peer closed).
- Hosts should treat `NotReady` as "try again soon", not a fatal error.
- Hosts may retry the same Write request after a transport timeout or lost
  response. TCP backends may acknowledge the immediately previous successful
  Write again only when handle, offset, length, and data bytes match exactly.
  Hosts must still advance their local write offset only after receiving and
  validating the response.
- Transport-layer framing limits (e.g. USB, SLIP, CDC buffers) MUST NOT affect protocol semantics.
  Hosts should assume that large responses require multiple Read calls.

#### Read size guarantees
- `dataLen` in the response MUST be `<= maxBytes` from the request.
- The device MUST NOT return more than `maxBytes` of data in a single Read response.
- If more data is available, the host MUST issue subsequent Read requests with an updated offset.

---

## Command: Info (0x05)

Fetch response metadata (HTTP status, content length, optional headers) **or** protocol-defined metadata for non-HTTP schemes.
This can be called repeatedly; results may become available over time.

- content_length is optional:
  - If the backend knows it (e.g., Content-Length header), it reports it.
  - For chunked/streaming responses, it may be 0/absent until completion or unknown entirely.

### Request
```
u8 version
u16 handle          // LE
```

### Response
```
u8 version
u8 flags            // bit0=headersIncluded, bit1=hasContentLength, bit2=hasHttpStatus
u16 reserved        // = 0
u16 handle          // LE
u16 httpStatus      // LE; valid only if hasHttpStatus
u64 contentLength   // LE; valid only if hasContentLength
u16 headerBytesLen  // LE; number of header bytes returned
u8[] headerBytes    // raw "Key: Value\r\n" bytes (only requested headers)
```

### Status codes
- `Ok`
- `NotReady` (metadata not available yet)
- `InvalidRequest`
- `IOError`

Notes:
- `headerBytes` is intentionally unstructured to keep parsing simple for 8-bit hosts.
- Modern tooling can parse it easily.
- If no response headers were requested in `Open()`, `headerBytesLen` MUST be 0 and
  `headersIncluded` MUST be 0. This is not an error.
- `Ok` with `dataLen == 0` MUST only be returned when `eof == true`.
  Returning `Ok` with `dataLen == 0` and `eof == false` is invalid.


### Protocol-specific interpretation

The Info wire format is stable, but interpretation is scheme-specific:

- **HTTP/HTTPS**:
  - `hasHttpStatus=1` when HTTP status is known, and `httpStatus` carries the HTTP status code.
  - `hasContentLength=1` when the response length is known.
  - `headerBytes` contains HTTP response headers ("Key: Value\r\n").

- **TCP (tcp://)**:
  - `hasHttpStatus=0` and `hasContentLength=0`.
  - `headerBytes` (if requested) contains **pseudo headers** that expose TCP state and counters
    without changing the v1 wire format.
  - See `docs/network_device_tcp.md` for the pseudo header keys and semantics.

### Info / Read ordering
- `Info()` and `Read()` are independent.
- `Info()` MAY return `Ok` before any body data is readable.
- `Read()` MAY return `NotReady` even after `Info()` succeeds.
- Hosts MUST treat both commands as independently retryable.

### TCP scheme behavior

For the TCP scheme:

- HTTP-specific fields (httpStatus, contentLength) are not meaningful and are
  reported as absent.
- Connection state and stream status are exposed via pseudo-headers in the
  headers block.

Implementations MAY include the following headers:

- X-FujiNet-Scheme: tcp
- X-FujiNet-Remote: host:port
- X-FujiNet-Connecting: 0|1
- X-FujiNet-Connected: 0|1
- X-FujiNet-PeerClosed: 0|1
- X-FujiNet-RxAvailable: <bytes currently buffered>
- X-FujiNet-ReadCursor: <current stream read offset>
- X-FujiNet-WriteCursor: <current stream write offset>
- X-FujiNet-LastError: <platform errno value or 0>

These headers allow TCP connection state to be queried without breaking v1
compatibility.

See docs/network_device_tcp.md for authoritative TCP definitions.

---

## Command: Ready (0x08)

Which of the requesting client's sessions can be read or queried right now. One small request replaces a
speculative `Read` or `Info` on each handle: a client with several connections waits (on its own timer, or on a
hardware hint such as the Atari's PROC line) and then asks once, instead of trying each. Only the requesting
client's sessions are listed (sessions are owned by the transport that opened them), and only ready ones.

### Request
```
u8 version
```

### Response
```
u8 version
u8 flags        // 0
u16 reserved    // = 0
u8 count        // number of entries
entry[count]:
  u16 handle    // LE
  u8  ready     // bit0 = read_ready (a Read would not be NotReady: bytes held, end of stream, or an error)
                // bit1 = info_ready (an Info would answer; HTTP: the status and headers have arrived)
  u16 available // LE; bytes held for the handle (saturating); 0 when the protocol cannot say
```

- A session that is still waiting for its request body (POST/PUT) is not listed.
- A `Ready` counts as activity for every session of the client, so a client that only waits on `Ready` is
  not idle for the idle timeout (see open flag `keep`).
- It is a hint, like the Atari PROC pulse: an entry can be early, or about data another request already took, and
  the client still answers a `NotReady` from `Read` or `Info`. A handle that was reaped or evicted is simply not
  listed; using it returns `InvalidRequest`.
- Firmware that predates the command answers `Unsupported`; the client falls back to trying each handle.

### Status codes
- `Ok` (possibly `count=0`)
- `InvalidRequest` (wrong version or trailing bytes)

---

## Command: Close (0x04)

Release resources for a handle.

### Request

```
u8   version
u16  handle              // LE
```

### Response

Optional minimal response payload:

```
u8  version
u8  flags=0
u16 reserved=0
```

### Status codes

- `Ok`
- `InvalidRequest` (unknown handle)

---

## HTTP request lifecycle (HTTP/HTTPS schemes)

### Overview

HTTP requests are created by `Open()`.

If the request has a body (POST/PUT and optional others), the body is streamed via `Write()` calls.
The request is dispatched automatically when the body is complete (or immediately for no-body requests).

Response data is retrieved with `Read()`.

### Body-required methods

For HTTP:
- POST and PUT MAY require a body (depending on `bodyLenHint`).
- GET/HEAD/DELETE typically have no body (we do not rely on a body for these in v1).

### Dispatch / commit rule (authoritative)

- If `bodyLenHint == 0`:
  - For GET/HEAD/DELETE (and POST/PUT with no body): the request MUST be dispatched immediately during `Open()`.
  - For POST/PUT with `body_unknown_len=1`: the request MUST NOT be dispatched during `Open()`.
    The body is streamed via `Write()` and committed by a zero-length `Write()`.

- If `bodyLenHint > 0`:
  - `Open()` MUST return `needs_body_write=1` in the Open response flags.
  - The client MUST send body bytes via one or more `Write(handle, offset, bytes)` calls.
  - The request MUST be dispatched automatically once the device has received exactly `bodyLenHint` bytes total for that handle.
  - `Read()` / `Info()` before dispatch completion MUST return `NotReady` (unless an error occurs).

### Unknown-length body commit (HTTP POST/PUT)

If `body_unknown_len=1` and `bodyLenHint==0`:

- `Open()` MUST return `needs_body_write=1`.
- The client MUST stream body bytes via sequential `Write(handle, offset, bytes)` calls.
- The client MUST commit the body by issuing a **zero-length** `Write(handle, finalOffset, dataLen=0)`.
  - `finalOffset` MUST equal the next expected sequential offset.
- `Read()` / `Info()` before commit MUST return `NotReady` (unless an error occurs).

### Write() rules for HTTP body streaming

- Offsets:
  - Offsets MUST be sequential (0..N) for HTTP request bodies.
  - If a `Write()` offset is non-sequential (gap or rewind), the device MUST return `InvalidRequest`.

- Length:
  - Total bytes written MUST NOT exceed `bodyLenHint`.
    If it would exceed, the device MUST return `InvalidRequest`.

- Backpressure:
  - If the device cannot accept more body bytes right now, `Write()` MAY return `DeviceBusy`.

### Errors during upload / dispatch

If the body upload fails or dispatch fails, subsequent `Info/Read` MUST return an appropriate error
(typically `IOError`) and the handle remains closeable.

### Contrast: TCP stream lifecycle (tcp scheme)

TCP uses the same v1 commands (Open/Write/Read/Info/Close) but differs from HTTP:

- There is no HTTP method or request/response concept.
- `Write()` maps to streaming `send()` and is not gated by `needs_body_write`.
- `Read()` maps to streaming `recv()`.
- Offsets are still required by the v1 protocol, but for TCP they are interpreted as
  **monotonic stream cursors** (sequential offsets only).
- `Info()` uses `headerBytes` for **pseudo headers** that expose TCP state (connected, bytes available, last error, etc.).

See `docs/network_device_tcp.md` for the full TCP mapping and pseudo header keys.

---

## Command: TranslateConfigure (0x07)

Command `0x07` exposes the generic translation configuration path.

### Request

```
u8   version
u16  handle
u8   translationType
u8   translationFlags
lp_u16 selector
```

### Response

```
u8   version
u8   flags             // bit0=result available
u16  reserved          // = 0
u16  handle
u32  translatedSize    // size of the translated result
```

### Semantics

- Reconfigures the handle to use the requested translator and selector.
- If the raw response body has already been cached, the device re-runs translation without refetching.
- Subsequent `Read` calls return translated bytes for the active selector.
- In the current implementation, `Json` and `Image` are supported, `Xml`/`Rss` return `Unsupported`, and an unknown type returns `InvalidRequest`.
- The `Image` translator reads the cached body in place, so no second copy of the body is made. `Json` still copies it in (the default `translate()`).

---

## Chunking Responsibility

Chunking is **host-driven**:

- Host repeatedly calls `Read`/`Write` with increasing offsets.
- Device does **no unsolicited continuation**.
- Device may return `DeviceBusy` to apply backpressure (host retries).

This model scales to:
- large HTTP downloads
- disk images fetched over HTTP
- constrained RAM platforms

---

## Implementation Notes (Device)

### Session table
NetworkDevice should keep a fixed-size table of sessions (small number, e.g. 4–8):

- active flag
- method/url/headers
- body-written tracking
- response-ready tracking
- HTTP status, content length, headers buffer
- response byte source (stream/buffer)
- eof flag
- timeouts / last activity

### poll()
`NetworkDevice::poll()` should:
- advance in-flight sessions
- perform network I/O
- fill response buffers progressively
- update NotReady vs Ok outcomes for `Read`/`Info`

---

### Handle

`handle` is a 16-bit opaque session identifier. Internally it encodes:
- a small session slot index (bounded by MAX_SESSIONS), and
- a generation counter to detect stale handles after a slot is reused.

Implications:
- Handle values are not sequential, and may look like multiples of 256, etc.
- Max concurrent sessions is bounded by MAX_SESSIONS; OPEN returns DeviceBusy when full.
- A handle may become invalid after CLOSE (or timeout reaping), even if the numeric value is later reused with a new generation.
- Open does not target an existing handle. Handles are allocated; they are not reused.

---

## Protocol-specific extensions

The NetworkDevice protocol is designed to support multiple network
protocols using the same command set and wire formats.

Protocol-specific behavior is documented separately:

- **TCP stream sockets**: see `docs/network_device_tcp.md`

These documents describe how each protocol maps its semantics onto the
core Open / Read / Write / Info / Close commands.
