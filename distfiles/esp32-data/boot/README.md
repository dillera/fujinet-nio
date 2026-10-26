Boot/config disk assets are generated here for ESP32 LittleFS uploadfs images.

Files in this directory are packaged into the ESP32 `storage` partition when
`./build.sh -f` runs through PlatformIO. All platforms' images together don't
fit the partition, so choose the ones a board needs in the `[fujinet]` section
of `platformio.local.ini`, as paths under `boot/` (a platform directory or a
single file), separated by spaces or commas:

```ini
[fujinet]
boot_images = amiga/wb32
; boot_images = bbc/FN-BOOT.ssd
; boot_images = msdos
```

Unset, empty or `none` packs no images: a board with no setting flashes no boot
disk. `all` packs everything, which is more than the storage partition of the
16 MB boards holds (about 4 MiB), so the build stops; name what the board needs.
`FN_BOOT_IMAGES=... ./build.sh -f` overrides the setting for one build. The
build lists what it packs, and stops before uploading if a name doesn't exist or
the selection won't fit (`pio-build/scripts/stage-fs-data.py`).
Do not commit generated `.atr`, `.img`, or `.ssd` files from this directory.
Generate them from the workspace boot-disk build tasks when packaging a local
build or release.

BBC FujiBus hosts use `bbc/FN-BOOT.ssd`; on ESP32 set:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/bbc/FN-BOOT.ssd"
  readonly: true
```

Amiga default disks are profile-specific FFS/DD ADFs.  After flashing the
storage image, select one explicitly, for example:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/amiga/wb13/FujiNet-Default-WB13.adf"
  readonly: true
```

Atari boards select `atari` (`./build.sh -s atari-...` sets it) and boot the disk
from flash. The ESP32 Atari NIO build already defaults `boot.config_uri` to
`flash:/boot/atari/autorun.atr`, so a fresh device needs no yaml. The name is
explicit on purpose: `persist:` resolves to the host folder, then the SD card,
then flash, so with an SD card inserted it would look there instead.

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/atari/autorun.atr"
  readonly: true
```

Other builds default `config_uri` to `persist:/boot/autorun.img`, which matches
none of the per-platform images here, so set it. Without editing files, use the
console (a restart applies it):

```
boot.get
boot.set config_uri flash:/boot/atari/autorun.atr
boot.save
```

A default only applies where `fujinet.yaml` does not say otherwise. A device that
already ran with the generic default has it saved in `fujinet.yaml` (on the SD
card when one is mounted, otherwise in flash, mirrored), so set it with
`boot.set` there. `./build.sh -f` rewrites the whole flash filesystem, so a
`fujinet.yaml` kept in flash is lost by it, and the profile default applies again.
