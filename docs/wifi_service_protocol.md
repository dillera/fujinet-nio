# Wi-Fi Management Service Protocol

The Wi-Fi management service is wire device `0xF3` and is registered when the
platform can expose the management endpoint. Hardware capabilities describe
which live operations are available; they do not by themselves hide the
endpoint on POSIX or emulator platforms.
Payload integers are little-endian. Every request and response starts with
protocol version `1`. Password bytes are accepted by `SET_CONFIG` and `TRY_JOIN`;
they are never present in any response.

## Commands

| Command | Value | Request |
| --- | ---: | --- |
| `GET_STATUS` | `0x01` | version only |
| `GET_CONFIG` | `0x02` | version only |
| `SET_CONFIG` | `0x03` | version, field flags, selected fields |
| `SCAN` | `0x04` | version, offset `u16`, limit `u8` |
| `GET_ADAPTER_INFO` | `0x05` | version (highest understood; see below) |
| `TRY_JOIN` | `0x06` | version, SSID string8, password string8 |
| `GET_JOIN_STATUS` | `0x07` | version (highest understood) |

`SET_CONFIG` field flags are `enabled=0x01`, `ssid=0x02`, `bssid=0x04`,
`password=0x08`, `persist=0x10`, and `reconnect=0x20`. Enabled is one byte;
strings are `u8` length followed by bytes. SSID is limited to 32 bytes,
password to 64 bytes, and BSSID is the canonical six-byte colon-separated
text form (`aa:bb:cc:dd:ee:ff`). Updates are validated before configuration
mutation. `persist` saves the complete configuration; `reconnect` connects or
disconnects after the update.

`GET_CONFIG` returns version, enabled, password-present, SSID, and configured
BSSID. `GET_STATUS` returns version, link state, configured-enabled,
current-BSSID-present, scan-supported, signed RSSI, current BSSID, capability
flags (`u16`), backend kind (`u8`), and four bounded IPv4 strings: address,
subnet, gateway, and DNS. The capability flags and backend kind are appended
after the four strings as a version-1 extension so older clients can parse the
original status fields. Link state values are
`0=disconnected`, `1=connecting`, `2=connected`, and `3=failed`.

Capability flags are config `0x0001`, status `0x0002`, connect `0x0004`,
disconnect `0x0008`, scan `0x0010`, BSSID selection `0x0020`, host-managed
`0x0040`, and simulated `0x0080`. Backend kinds are unavailable `0`, ESP32
`1`, POSIX host-managed `2`, and POSIX simulated `3`.

`GET_ADAPTER_INFO` returns version, MAC-present (`u8`) and the six-byte station
interface MAC address (zero when not present). Unlike the older commands here,
it follows the [extending-commands rules](protocol_reference.md#extending-commands):
the client sends the highest version it understands, the reply uses the lower
of that and the firmware's (`1` today), and bytes after the version are
ignored. It succeeds without a live link;
the MAC is then reported as not present. The ESP32 backend reports the factory
station MAC (`esp_read_mac`), POSIX host mode reads
`/sys/class/net/<FN_POSIX_WIFI_INTERFACE>/address` on Linux, and POSIX
simulated mode reports `02:00:00:00:00:f0`. Firmware without this command
returns `Unsupported`, so clients should show the MAC as unavailable rather
than fail. The firmware version comes from FujiDevice `GetInfo`
([FujiDevice protocol](fuji_device_protocol.md)), not from this service. The
console's `net.wifi.status` shows the MAC, and `./scripts/fujinet --port <port>
wifi adapter` asks for it from a host.

`TRY_JOIN` tests new credentials before saving them. If the connected link
provides its credentials and they differ from saved settings (for example after
an older client saved settings without reconnecting), the working live settings
are first saved as the restoration point. Otherwise the saved settings are used. The SSID is 1–32 bytes, the password is 0–64 bytes; an empty
password selects an open network. It clears any old BSSID pin and starts the
attempt after sending its reply. The old credentials stay saved until the new
link reports Connected. On success the new credentials are saved. On failure
or after 30 seconds, the service retains the old configuration and attempts to
reconnect it (or disconnects if none was configured). A restart during a trial
therefore retains the previous configuration. A concurrent `TRY_JOIN`, `SET_CONFIG`, or `SCAN`
is rejected as busy; console scan and set commands are likewise blocked during
the trial. Passwords are never returned.

`GET_JOIN_STATUS` returns version and a state byte: 0 idle, 1 pending, 2
connected and saved, 3 failed with restoration attempted. The failure result
is retained even if the old network subsequently reconnects; inspect the live
link separately with `GET_STATUS`. The console's `net.wifi.status` also reports
`join_trial`. These new commands
follow the [extension rules](protocol_reference.md#extending-commands): they
accept a nonzero highest-supported version and ignore trailing request bytes;
replies use the lower supported version. Older firmware returns Unsupported.
`wifi join <ssid>` starts a trial (prompting for the password; `--open` selects
an open network); `wifi join-status` reports the result.

`SCAN` returns version, more flag, count, and records. Each record contains an
SSID, six BSSID bytes, signed RSSI, channel, and authentication mode. The
offset and limit make responses safe for 8-bit clients; implementations cap a
single response at 32 records.

Malformed payloads return `InvalidRequest`; unavailable live-link operations
return `NotReady`; scan failures return `IOError`; invalid BSSID text returns
`InvalidRequest`; unknown commands return `Unsupported`.

## POSIX Backends

The POSIX FujiNet process always registers this service. Set
`FN_POSIX_WIFI_BACKEND` to `simulated` (the deterministic default), `host`, or
`unavailable`. Host mode observes the selected host interface without taking
control of the host network; set `FN_POSIX_WIFI_INTERFACE` to select an
interface. Simulated mode provides three stable access points and a test IPv4
connection, making PTY and BBC emulator tests independent of host Wi-Fi. For
failure-path tests, `FN_POSIX_WIFI_SIM_RESTORE_DELAY_MS` delays a simulated
restoration after a rejected connection, and `FN_POSIX_WIFI_SIM_SCAN_EDGE_CASES`
adds a hidden SSID and duplicate access point. `FN_POSIX_WIFI_SIM_FAIL_SSID` makes that named simulated
network fail to connect; it has no effect on the host-managed backend.

## What Was Implemented

The implementation follows the original plan with these explicit decisions:

- `WifiService` remains separate from `NetworkDevice`, while configuration,
  persistence, reconnect checks, BSSID validation, and scan access are shared
  through `WifiController` by both the wire service and console diagnostics.
- Registration is based on service/backend availability rather than only
  `managesItsOwnLink`. This is intentional: POSIX PTY and BBC emulator runs
  must expose the same service as ESP32. POSIX capabilities identify whether
  the backend is host-managed, simulated, or unavailable.
- Scan offset `0` refreshes the service cache. Later offsets consume that cache,
  so paginated clients see one consistent scan without repeatedly starting a
  radio scan.
- Capability flags and backend kind are appended after the original version-1
  status fields. This is a wire-layout choice for forward parsing within the
  unreleased protocol, not compatibility with an external released client.
- POSIX host mode observes the host network and does not attempt to disconnect
  or reconfigure it. POSIX simulated mode is deterministic and is the default
  for emulator/PTY testing.
- BBC library calls use the existing `fn_bbc_device_call_raw` ABI. No new
  assembly wrapper was needed for these helpers because the existing ROM
  device-call ABI already carries arbitrary device and command payloads.
  The BBC implementation uses one 512-byte static scan response buffer because
  that ABI has no caller-supplied response scratch argument; this is the only
  persistent Wi-Fi library buffer and is required to preserve the public API.
- Authentication mode is represented as a compact numeric wire value. Console
  diagnostics print that value rather than duplicating platform-specific
  ESP32 authentication labels.
