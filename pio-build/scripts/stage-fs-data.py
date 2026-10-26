# PlatformIO pre-script: choose which boot images go into the flash filesystem.
#
# distfiles/esp32-data/boot holds the images for every platform, more than the
# storage partition can hold. Set `boot_images` in the [fujinet] section of
# platformio.local.ini to the ones this board needs, as paths under boot/
# (a platform directory or a single file), separated by spaces or commas:
#
#   [fujinet]
#   boot_images = amiga/wb32
#
# Unset or empty packs no images (every image together does not fit the storage
# partition, so flashing none is the safe default); "none" says so explicitly and
# "all" packs everything, which fails if it does not fit. FN_BOOT_IMAGES in the
# environment overrides the setting for one build. The selection is copied
# to $BUILD_DIR/fs-data, which becomes the filesystem image's source, and the
# build stops if it won't fit the storage partition.

Import("env")  # noqa: F821 (SCons)

import csv
import os
import re
import shutil
import sys
import time

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}
BLOCK = 4096


def fail(message):
    sys.stderr.write(f"[boot_images] {message}\n")
    env.Exit(1)  # noqa: F821


def selection():
    raw = os.environ.get("FN_BOOT_IMAGES")
    source = "FN_BOOT_IMAGES"
    if raw is None:
        config = env.GetProjectConfig()  # noqa: F821
        if config.has_option("fujinet", "boot_images"):
            raw = config.get("fujinet", "boot_images")
            source = "platformio.local.ini [fujinet] boot_images"
        else:
            raw = ""
            source = "boot_images not set"
    items = [s.strip().strip("/") for s in re.split(r"[\s,]+", raw) if s.strip()]
    return items, source


def available(boot_dir):
    found = []
    for root, _, files in os.walk(boot_dir):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), boot_dir)
            if os.sep in rel:  # files directly in boot/ (README.md) always go in
                found.append(rel.replace(os.sep, "/"))
    return sorted(found)


def littlefs_bytes(root):
    """Rough LittleFS footprint: whole blocks per file, a block pair per entry."""
    total = 2 * BLOCK  # superblock pair
    for dirpath, dirs, files in os.walk(root):
        total += 2 * BLOCK * (len(dirs) + len(files))
        for name in files:
            size = os.path.getsize(os.path.join(dirpath, name))
            total += -(-size // BLOCK) * BLOCK
    return total


def fs_partition_size():
    board = env.BoardConfig()  # noqa: F821
    config = env.GetProjectConfig()  # noqa: F821
    section = "env:" + env.subst("$PIOENV")  # noqa: F821
    table = board.get("build.partitions", "")
    if config.has_option(section, "board_build.partitions"):
        table = config.get(section, "board_build.partitions")
    path = os.path.join(env.subst("$PROJECT_DIR"), table)  # noqa: F821
    if not table or not os.path.isfile(path):
        return None
    with open(path, newline="") as f:
        for row in csv.reader(line for line in f if not line.lstrip().startswith("#")):
            cells = [c.strip() for c in row]
            if len(cells) >= 5 and cells[1] == "data" and cells[2] in ("littlefs", "spiffs", "fat"):
                return int(cells[4], 0)
    return None


def stage():
    data_dir = env.subst("$PROJECT_DATA_DIR")  # noqa: F821
    boot_dir = os.path.join(data_dir, "boot")
    items, source = selection()
    everything = items == ["all"]
    images = available(boot_dir) if os.path.isdir(boot_dir) else []

    if not everything:
        if items == ["none"]:
            items = []
        missing = [i for i in items if not os.path.exists(os.path.join(boot_dir, i))]
        if missing:
            listing = "\n  ".join(images) or "(none: build them with ./scripts/build.sh boot-disks)"
            fail(f"not found under {boot_dir}: {', '.join(missing)} (from {source})\n"
                 f"Available:\n  {listing}")
            return

    staged = os.path.join(env.subst("$BUILD_DIR"), "fs-data")  # noqa: F821
    shutil.rmtree(staged, ignore_errors=True)
    if everything:
        shutil.copytree(data_dir, staged)
    else:
        # Everything outside boot/, the files directly in boot/, then the selection.
        shutil.copytree(data_dir, staged, ignore=lambda d, names: ["boot"] if d == data_dir else [])
        os.makedirs(os.path.join(staged, "boot"), exist_ok=True)
        if os.path.isdir(boot_dir):
            for name in os.listdir(boot_dir):
                if os.path.isfile(os.path.join(boot_dir, name)):
                    shutil.copy2(os.path.join(boot_dir, name), os.path.join(staged, "boot", name))
        for item in items:
            src = os.path.join(boot_dir, item)
            dst = os.path.join(staged, "boot", item)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isdir(src):
                shutil.copytree(src, dst, dirs_exist_ok=True)
            else:
                shutil.copy2(src, dst)

    chosen = "all" if everything else (", ".join(items) or "none")
    if not everything and not items and source == "boot_images not set":
        chosen = "none (set boot_images in platformio.local.ini to flash a boot disk, e.g. boot_images = atari)"
    print(f"[boot_images] {chosen} (from {source})")
    # Build times make a stale image obvious before it is flashed.
    for image in available(os.path.join(staged, "boot")):
        built = time.strftime("%Y-%m-%d %H:%M", time.localtime(
            os.path.getmtime(os.path.join(staged, "boot", image))))
        print(f"[boot_images]   boot/{image} (built {built})")

    needed = littlefs_bytes(staged)
    capacity = fs_partition_size()
    if capacity is not None and needed > capacity:
        hint = "choose fewer images with boot_images"
        fail(f"selection needs about {needed // 1024} KiB but the storage partition holds "
             f"{capacity // 1024} KiB; {hint}")
        return
    env.Replace(PROJECT_DATA_DIR=staged)  # noqa: F821


if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # noqa: F821
    stage()
