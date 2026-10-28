#include "fujinet/disk/xex_boot_image.h"

#include <algorithm>
#include <cstring>

namespace fujinet::disk {

// The PicoBoot boot code below (sectors 1-3, 3 x 128 bytes) is
// picobootcode-dd.bin from MyPicoDOS 4.06, Copyright (C) 1992-2017 Matthias
// Reichl, licensed under the GNU General Public License version 2 (or, as
// this project is GPL, later). It is reproduced unmodified; see THIRD_PARTY.md.

namespace {

constexpr std::uint8_t kPicoBoot[384] = {
    0x00, 0x03, 0x00, 0x08, 0x3d, 0x09, 0xa2, 0x7d, 0xad, 0x00, 0x08, 0xd0, 0x08, 0x8d, 0x08, 0x03,
    0xee, 0x09, 0x03, 0xa2, 0xfd, 0x86, 0x48, 0xa2, 0x00, 0x8e, 0x04, 0x03, 0xe8, 0x8e, 0x0b, 0x03,
    0xa9, 0x07, 0x8d, 0x05, 0x03, 0xa9, 0x69, 0x8d, 0x0a, 0x03, 0xa9, 0x40, 0x8d, 0x03, 0x03, 0x20,
    0x59, 0xe4, 0xad, 0x00, 0x07, 0x29, 0x04, 0xf0, 0x05, 0xa9, 0xff, 0x8d, 0x2b, 0x09, 0xa4, 0x48,
    0xad, 0x03, 0x07, 0x99, 0x01, 0x07, 0xad, 0x04, 0x07, 0x99, 0x00, 0x07, 0xa9, 0x3d, 0x8d, 0xe2,
    0x02, 0xa9, 0x09, 0x8d, 0xe3, 0x02, 0x20, 0x3e, 0x09, 0xa2, 0x02, 0x20, 0x32, 0x09, 0x85, 0x44,
    0x8d, 0xe0, 0x02, 0x20, 0x32, 0x09, 0x85, 0x45, 0x8d, 0xe1, 0x02, 0x20, 0x32, 0x09, 0x38, 0xe5,
    0x44, 0x85, 0x46, 0x08, 0x20, 0x32, 0x09, 0x28, 0xe5, 0x45, 0x85, 0x47, 0xe6, 0x46, 0xe6, 0x47,
    0xa0, 0x00, 0xe4, 0x43, 0xd0, 0x5c, 0x20, 0x3e, 0x09, 0xa5, 0x47, 0xc9, 0x02, 0x90, 0x53, 0xa5,
    0x43, 0xc5, 0x48, 0xd0, 0x4d, 0x98, 0x18, 0x65, 0x44, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 0xa4,
    0x48, 0x88, 0xb9, 0x00, 0x07, 0x91, 0x44, 0x88, 0xb9, 0x00, 0x07, 0x91, 0x44, 0x88, 0xb9, 0x00,
    0x07, 0x91, 0x44, 0x88, 0xb9, 0x00, 0x07, 0x91, 0x44, 0x88, 0xd0, 0xe6, 0xb9, 0x00, 0x07, 0x91,
    0x44, 0x18, 0xa5, 0x44, 0x65, 0x48, 0x85, 0x44, 0x90, 0x02, 0xe6, 0x45, 0x38, 0xa5, 0x46, 0xd0,
    0x02, 0xe6, 0x47, 0xe5, 0x48, 0x85, 0x46, 0xd0, 0x02, 0xc6, 0x47, 0xb0, 0x02, 0xc6, 0x47, 0x4c,
    0x86, 0x08, 0xbd, 0x00, 0x07, 0xe8, 0x91, 0x44, 0xc6, 0x46, 0xd0, 0x04, 0xc6, 0x47, 0xf0, 0x07,
    0xc8, 0xd0, 0x8f, 0xe6, 0x45, 0xd0, 0x8b, 0xad, 0xe3, 0x02, 0xc9, 0x09, 0xf0, 0x18, 0x8e, 0x29,
    0x09, 0x20, 0x2c, 0x09, 0xad, 0x0b, 0xd4, 0xd0, 0xfb, 0xae, 0x29, 0x09, 0xa9, 0x3d, 0x8d, 0xe2,
    0x02, 0xa9, 0x09, 0x8d, 0xe3, 0x02, 0x20, 0x32, 0x09, 0x85, 0x44, 0x20, 0x32, 0x09, 0x85, 0x45,
    0x25, 0x44, 0xc9, 0xff, 0xf0, 0xf0, 0x4c, 0x6b, 0x08, 0x00, 0x00, 0x03, 0x6c, 0xe2, 0x02, 0x6c,
    0xe0, 0x02, 0xe4, 0x43, 0xd0, 0x03, 0x20, 0x3e, 0x09, 0xbd, 0x00, 0x07, 0xe8, 0x60, 0x8c, 0x2a,
    0x09, 0xa4, 0x48, 0xb9, 0x00, 0x07, 0x2d, 0x2b, 0x09, 0x8d, 0x0b, 0x03, 0xb9, 0x01, 0x07, 0x8d,
    0x0a, 0x03, 0x0d, 0x0b, 0x03, 0xf0, 0x19, 0xa9, 0x40, 0x8d, 0x03, 0x03, 0x20, 0x59, 0xe4, 0x30,
    0xf6, 0xa4, 0x48, 0xb9, 0x02, 0x07, 0x85, 0x43, 0xf0, 0xd4, 0xa2, 0x00, 0xac, 0x2a, 0x09, 0x60,
    0x68, 0x68, 0xa9, 0x01, 0x85, 0x09, 0x20, 0x2c, 0x09, 0x20, 0x2f, 0x09, 0x6c, 0x0a, 0x00, 0x00,
};

constexpr std::uint32_t kSectorSize = 256;
constexpr std::uint32_t kBootSectorSize = 128;
constexpr std::uint32_t kPayload = 253;       // data bytes per file sector
constexpr std::uint32_t kMinSectors = 720;
constexpr std::uint32_t kMaxSectors = 65535;  // 16-bit sector links / counts
constexpr std::uint32_t kVtoc1 = 360;         // first VTOC sector (1-based)
constexpr std::uint32_t kDirFirst = 361;
constexpr std::uint32_t kDirLast = 368;
constexpr std::uint32_t kFirstData = 4;
constexpr std::uint32_t kVtoc1Bits = 246 * 8; // sectors 0..1967 in VTOC 1 (bytes 10..255)
constexpr std::uint32_t kVtocNBits = 256 * 8; // later VTOC sectors
constexpr const char kName[11] = {'A','U','T','O','R','U','N',' ','X','E','X'};

// VTOC sectors needed for an N-sector disk (sector numbers 0..N).
std::uint32_t vtoc_count(std::uint32_t n) noexcept
{
    if (n + 1 <= kVtoc1Bits) return 1;
    return 1 + (n + 1 - kVtoc1Bits + kVtocNBits - 1) / kVtocNBits;
}

// dir2atr -m -d sizing: 720 sectors when the file fits (up to 707 data
// sectors), otherwise the smallest 8m+7 that holds boot (3), VTOC, directory
// (8) and the data.
bool compute_layout(std::uint64_t sizeBytes, std::uint32_t& k, std::uint32_t& n) noexcept
{
    if (sizeBytes == 0) return false;
    const std::uint64_t kk = (sizeBytes + kPayload - 1) / kPayload;
    if (kk > kMaxSectors) return false;
    k = static_cast<std::uint32_t>(kk);
    if (k <= 707) {
        n = kMinSectors;
        return true;
    }
    std::uint32_t v = 1;
    std::uint32_t cand = 0;
    for (int i = 0; i < 8; ++i) {
        std::uint32_t need = std::max<std::uint32_t>(k + 11 + v, kMinSectors);
        cand = (need / 8) * 8 + 7;
        const std::uint32_t nv = vtoc_count(cand);
        if (nv == v) break;
        v = nv;
    }
    if (cand > kMaxSectors) return false;
    n = cand;
    return true;
}

class XexBootDiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::XexBoot; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return true; }

    DiskResult mount(std::unique_ptr<fs::IFile> file, std::uint64_t sizeBytes, const MountOptions&) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};
        std::uint32_t k = 0, n = 0;
        if (!compute_layout(sizeBytes, k, n)) return DiskResult{DiskError::BadImage};
        std::uint8_t head[2]{};
        if (!file->seek(0) || file->read(head, 2) != 2) return DiskResult{DiskError::IoError};
        if (!xex_has_binary_header(head, 2)) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _size = sizeBytes;
        _k = k;
        _n = n;
        _v = vtoc_count(n);
        _filePos = 2;
        _geo.sectorSize = kSectorSize;
        _geo.sectorCount = n;
        _geo.supportsVariableSectorSize = true; // first three sectors are 128 bytes, as in ATR
        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _size = 0;
        _k = _n = _v = 0;
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!dst) return DiskResult{DiskError::InvalidSlot};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (dstBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint32_t s = lba + 1;
        std::memset(dst, 0, kSectorSize);
        if (s <= 3) {
            std::memcpy(dst, &kPicoBoot[(s - 1) * kBootSectorSize], kBootSectorSize);
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(kBootSectorSize)};
        }
        if (s >= kVtoc1 + 1 - _v && s <= kVtoc1) {
            fill_vtoc(kVtoc1 - s, dst);
        } else if (s == kDirFirst) {
            dst[0] = (_n > 1023) ? 0x46 : 0x42;
            dst[1] = static_cast<std::uint8_t>(_k & 0xff);
            dst[2] = static_cast<std::uint8_t>(_k >> 8);
            dst[3] = static_cast<std::uint8_t>(kFirstData & 0xff);
            dst[4] = static_cast<std::uint8_t>(kFirstData >> 8);
            std::memcpy(&dst[5], kName, sizeof(kName));
        } else {
            std::uint32_t j = 0;
            if (data_index(s, j) && j < _k) {
                const DiskResult r = fill_data(j, dst);
                if (!r.ok()) return r;
            }
        }
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(kSectorSize)};
    }

    DiskResult write_sector(std::uint32_t, const std::uint8_t*, std::size_t) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        return DiskResult{DiskError::ReadOnly};
    }

    DiskResult flush() override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        return DiskResult{DiskError::None};
    }

private:
    // Data sectors run 4..(360-V), skip the VTOC and directory, resume at 369.
    std::uint32_t first_run() const noexcept { return kVtoc1 - _v - kFirstData + 1; }

    std::uint32_t sector_of(std::uint32_t j) const noexcept
    {
        const std::uint32_t a = first_run();
        return (j < a) ? kFirstData + j : kDirLast + 1 + (j - a);
    }

    bool data_index(std::uint32_t s, std::uint32_t& j) const noexcept
    {
        if (s >= kFirstData && s <= kVtoc1 - _v) { j = s - kFirstData; return true; }
        if (s > kDirLast) { j = first_run() + (s - kDirLast - 1); return true; }
        return false;
    }

    bool sector_free(std::uint32_t x) const noexcept
    {
        if (x == 0 || x > _n) return false;
        std::uint32_t j = 0;
        return data_index(x, j) && j >= _k;
    }

    void fill_vtoc(std::uint32_t idx, std::uint8_t* dst) const noexcept
    {
        std::uint32_t firstSector = 0;
        std::uint32_t byteOff = 0;
        if (idx == 0) {
            const std::uint32_t total = _n - 11 - _v;
            dst[0] = (_n > 1023) ? static_cast<std::uint8_t>(2 + _v) : 2;
            dst[1] = static_cast<std::uint8_t>(total & 0xff);
            dst[2] = static_cast<std::uint8_t>(total >> 8);
            const std::uint32_t freeSec = total - _k;
            dst[3] = static_cast<std::uint8_t>(freeSec & 0xff);
            dst[4] = static_cast<std::uint8_t>(freeSec >> 8);
            byteOff = 10;
        } else {
            firstSector = kVtoc1Bits + (idx - 1) * kVtocNBits;
        }
        for (std::uint32_t b = byteOff; b < kSectorSize; ++b) {
            const std::uint32_t base = firstSector + (b - byteOff) * 8;
            std::uint8_t v = 0;
            for (std::uint32_t bit = 0; bit < 8; ++bit) {
                if (sector_free(base + bit)) v |= static_cast<std::uint8_t>(0x80u >> bit);
            }
            dst[b] = v;
        }
    }

    DiskResult fill_data(std::uint32_t j, std::uint8_t* dst)
    {
        const std::uint64_t off = static_cast<std::uint64_t>(j) * kPayload;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kPayload, _size - off));
        if (off != _filePos && !_file->seek(off)) { _filePos = ~0ull; return DiskResult{DiskError::IoError}; }
        if (_file->read(dst, want) != want) { _filePos = ~0ull; return DiskResult{DiskError::IoError}; }
        _filePos = off + want;
        const std::uint32_t next = (j + 1 < _k) ? sector_of(j + 1) : 0;
        dst[253] = static_cast<std::uint8_t>(next >> 8);
        dst[254] = static_cast<std::uint8_t>(next & 0xff);
        dst[255] = static_cast<std::uint8_t>(want);
        return DiskResult{DiskError::None};
    }

    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geo{};
    std::uint64_t _size{0};
    std::uint64_t _filePos{0};
    std::uint32_t _k{0}; // data sectors
    std::uint32_t _n{0}; // total sectors
    std::uint32_t _v{0}; // VTOC sectors
};

} // namespace

bool xex_has_binary_header(const std::uint8_t* head, std::size_t n) noexcept
{
    return n >= 2 && head[0] == 0xFF && head[1] == 0xFF;
}

bool xex_boot_geometry(std::uint64_t sizeBytes, DiskGeometry& out) noexcept
{
    std::uint32_t k = 0, n = 0;
    if (!compute_layout(sizeBytes, k, n)) return false;
    out.sectorSize = kSectorSize;
    out.sectorCount = n;
    out.supportsVariableSectorSize = true;
    return true;
}

std::unique_ptr<IDiskImage> make_xex_boot_disk_image()
{
    return std::make_unique<XexBootDiskImage>();
}

} // namespace fujinet::disk
