#pragma once

#include <cstdint>
#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Atari executable (.xex/.com/.exe) presented as a synthesised, read-only,
// bootable MyDOS disk. Boot sectors 1-3 are the MyPicoDOS PicoBoot loader;
// the executable is the disk's single file "AUTORUN XEX". Sector contents are
// computed on demand from the source file; nothing but one sector is buffered.
// The layout is byte-identical to `dir2atr -m -d -B picobootcode-dd.bin` for a
// stage directory holding only autorun.xex. See docs/disk_device_protocol.md.
std::unique_ptr<IDiskImage> make_xex_boot_disk_image();

// Geometry the synthesised disk has for an executable of sizeBytes (256-byte
// sectors, at least 720). False when the file is empty or too big to address.
bool xex_boot_geometry(std::uint64_t sizeBytes, DiskGeometry& out) noexcept;

// True when the first bytes are the Atari binary-load header (FF FF).
bool xex_has_binary_header(const std::uint8_t* head, std::size_t n) noexcept;

} // namespace fujinet::disk
