# Palm HotSync

FujiNet NIO can be the desktop end of a Palm OS HotSync. A Palm syncs with it
to receive queued apps and to have its databases backed up, the jobs Palm
Desktop used to do.

What works today:

- **Network HotSync (NetSync)** on TCP 14238, from a Palm's Network HotSync or
  from `scripts/fake_palm.py`, on POSIX and ESP32.
- A sync identifies the Palm's user (a Palm that has never synced is given a
  name and a new user ID), installs queued `.prc`, `.pdb` and `.pqa` files,
  backs up databases, and stamps the Palm as last synced with FujiNet.

Not yet: HotSync from a Palm in a serial cradle (the SLP, PADP and CMP layers
are in place and tested, but the cradle must share the bus line with FujiBus
apps first), and copying a calendar into the Date Book.

## Turning it on

HotSync is off by default. Add a `hotsync:` block to `fujinet.yaml` and restart:

```yaml
hotsync:
  enabled: true
  port: 14238          # NetSync TCP port
  root: /hotsync       # folder on the persistent filesystem
  user_name: FujiNet   # given to a Palm that has never synced
  backup: flagged      # none | flagged | all
```

Settings are read at start-up, so a change takes effect after a restart.

`backup` chooses what a sync copies off the Palm:

- `none`: nothing.
- `flagged`: databases whose backup bit is set, as Palm Desktop does.
- `all`: every RAM database.

"Unsaved Preferences" is never backed up; every Palm rebuilds it.

## Files

The folder lives on the persistent filesystem (`host` on POSIX, `sd0` or
`flash` on ESP32), under `root`:

| Folder | Contents |
|---|---|
| `install/` | Files to install at the next sync. FujiNet creates it at start-up. |
| `installed/` | Files move here once installed. |
| `backup/<user>/` | Databases backed up from the Palm, as `.pdb` or `.prc` |

Copy a file into `install/` through any route that reaches the persistent
filesystem: the host folder on POSIX, the SD card, or a FileDevice write from a
FujiNet client. File and user names are made FAT-safe: characters other than
letters, digits, space, `-`, `_` and `.` become `_`.

A database larger than 1 MB is neither installed nor backed up, so it never
has to be held in memory. On ESP32 buffers that large come from PSRAM.

## Syncing a Palm

On the Palm, open HotSync, choose **Modem Sync Prefs** → **Network**, set the
primary PC address to the FujiNet's IP address, and tap the Modem Sync icon.

Without a Palm, `scripts/fake_palm.py` plays one:

```sh
./scripts/fake_palm.py localhost --state palm.json
```

It keeps its user and databases in the state file, so a second run is a
returning Palm. A new state file holds one flagged database, "FujiNet Notes",
so the first sync with `backup: flagged` backs it up.

## Diagnostics

`hotsync.status` shows whether the service is on and listening, its settings,
how many syncs have run, and the result of the last one:

```
enabled: 1
listening: 1
port: 14238
root: /hotsync
user_name: FujiNet
backup: flagged
syncing: 0
syncs: 1
failures: 0
last_time: 2026-10-18 13:02:12
last_user: Test Palm
last_result: ok
last_installed: 1
last_install_failures: 0
last_backed_up: 1
last_backup_failures: 0
```

## Design

- The protocol stack lives in `fujinet::hotsync` (`include/fujinet/hotsync/`,
  `src/lib/hotsync/`) and depends on no platform, bus or device:
  - `Slp`, `PadpTransport` (with CMP): the serial stack.
  - `NetSyncTransport`: the network framing and handshake.
  - `DlpClient`: Desktop Link Protocol commands.
  - `PalmDatabase`: `.pdb` and `.prc` files.
  - `Session`: one sync, over injected `DlpTransport` and `Storage` interfaces.
- `NetSyncService` listens through `net::ITcpSocketOps` and runs one session
  per connection. A sync waits on the Palm for seconds at a time and runs for
  minutes, so the service runs on a task of its own
  (`platform::start_service_task`), never in `core.tick()`.
  `hotsync::start_netsync_service()` starts it from the platform `main`.
- `FsStorage` keeps the folders above on an `fs::IFileSystem`.
- Tests: `tests/test_hotsync.cpp` replays bytes captured from a Palm OS 3.3
  device and palm-sync's NetSync recording, and runs whole syncs against a
  fake Palm.

## Licence

The protocol code in `src/lib/hotsync/` marked "Derived from palm-sync" is
based on [palm-sync](https://github.com/jichu4n/palm-sync) by Chuan Ji,
licensed under the Apache License, Version 2.0. You may obtain a copy of the
licence at <https://www.apache.org/licenses/LICENSE-2.0>. Those files have been
modified from the original: rewritten in C++ for FujiNet NIO. No pilot-link
(GPL) code is used.
