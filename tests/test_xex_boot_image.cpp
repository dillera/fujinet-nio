#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_probers/image_probe.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/disk/xex_boot_image.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/disk_codec.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef FN_XEX_FIXTURE_DIR
#error "FN_XEX_FIXTURE_DIR must be defined by tests/CMakeLists.txt"
#endif

using namespace fujinet::disk;
using fujinet::tests::MemoryFile;

namespace {

// Same content as synth() in tests/fixtures/xex-boot/generate.py.
std::vector<std::uint8_t> synth(std::size_t size)
{
    std::vector<std::uint8_t> b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(i * 131 + (i >> 8) * 7 + 13);
    b[0] = b[1] = 0xFF;
    return b;
}

std::uint64_t fnv1a(std::uint64_t h, const std::uint8_t* p, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

std::vector<std::uint8_t> from_hex(const std::string& s)
{
    std::vector<std::uint8_t> v;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return v;
}

struct Golden {
    std::size_t size{0};
    std::uint32_t sectors{0};
    std::uint64_t fnv{0};
    std::map<std::uint32_t, std::vector<std::uint8_t>> sector;
};

Golden load_golden(const std::string& name)
{
    std::ifstream in(std::string(FN_XEX_FIXTURE_DIR) + "/" + name + ".txt");
    REQUIRE(in.good());
    Golden g;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "size") ls >> g.size;
        else if (key == "sectors") ls >> g.sectors;
        else if (key == "fnv") { std::string h; ls >> h; g.fnv = std::stoull(h, nullptr, 16); }
        else if (key == "sector") { std::uint32_t n; std::string h; ls >> n >> h; g.sector[n] = from_hex(h); }
    }
    return g;
}

std::unique_ptr<IDiskImage> mount_bytes(std::vector<std::uint8_t>& bytes, DiskResult* out = nullptr)
{
    auto image = make_xex_boot_disk_image();
    MountOptions opts{};
    const auto r = image->mount(std::make_unique<MemoryFile>(bytes, true, nullptr, nullptr), bytes.size(), opts);
    if (out) *out = r;
    if (!r.ok()) return nullptr;
    return image;
}

// 1-based sector number -> bytes as the ATR payload stores them (128-byte
// boot sectors, 256 after).
std::vector<std::uint8_t> read_1based(IDiskImage& img, std::uint32_t s)
{
    std::uint8_t buf[256];
    const auto r = img.read_sector(s - 1, buf, sizeof(buf));
    REQUIRE(r.ok());
    return std::vector<std::uint8_t>(buf, buf + r.bytes);
}

const std::uint8_t kPicoBootHead[16] = {0x00, 0x03, 0x00, 0x08, 0x3d, 0x09, 0xa2, 0x7d,
                                        0xad, 0x00, 0x08, 0xd0, 0x08, 0x8d, 0x08, 0x03};

} // namespace

TEST_CASE("xex boot: probe by extension and by FF FF header")
{
    auto xex = synth(1000);
    for (const char* path : {"/a.xex", "/A.XEX", "/a.com", "/a.exe", "/noext", "/a.bin"}) {
        MemoryFile file(xex, true, nullptr, nullptr);
        const auto r = probe_image(file, xex.size(), path, MountOptions{});
        CAPTURE(path);
        CHECK(r.matched);
        CHECK(r.type == ImageType::XexBoot);
        CHECK(r.geometry.sectorSize == 256);
        CHECK(r.geometry.sectorCount == 720);
    }
    // Extension without header still selects the type (the mount then fails clearly).
    auto notXex = std::vector<std::uint8_t>(1000, 0x41);
    MemoryFile f2(notXex, true, nullptr, nullptr);
    CHECK(probe_image(f2, notXex.size(), "/a.xex", MountOptions{}).type == ImageType::XexBoot);
    MemoryFile f3(notXex, true, nullptr, nullptr);
    CHECK(probe_image(f3, notXex.size(), "/a.bin", MountOptions{}).type != ImageType::XexBoot);

    // A blank 0xFF-filled disk image is not an executable.
    auto blank = std::vector<std::uint8_t>(720 * 256, 0xFF);
    for (const char* path : {"/blank.img", "/blank.raw", "/blank.xfd", "/blank.dsk"}) {
        MemoryFile f(blank, true, nullptr, nullptr);
        CAPTURE(path);
        CHECK(probe_image(f, blank.size(), path, MountOptions{}).type != ImageType::XexBoot);
    }
}

TEST_CASE("xex boot: sector count rules")
{
    DiskGeometry g{};
    CHECK(!xex_boot_geometry(0, g));
    REQUIRE(xex_boot_geometry(1, g));
    CHECK(g.sectorCount == 720);
    REQUIRE(xex_boot_geometry(707 * 253, g));
    CHECK(g.sectorCount == 720);
    REQUIRE(xex_boot_geometry(707 * 253 + 1, g));
    CHECK(g.sectorCount == 727);
    REQUIRE(xex_boot_geometry(1011 * 253, g));
    CHECK(g.sectorCount == 1023);
    REQUIRE(xex_boot_geometry(1011 * 253 + 1, g));
    CHECK(g.sectorCount == 1031);
    REQUIRE(xex_boot_geometry(300059, g)); // dir2atr: 1199
    CHECK(g.sectorCount == 1199);
    CHECK(g.sectorSize == 256);
    CHECK(!xex_boot_geometry(65536ull * 253, g));
}

TEST_CASE("xex boot: output is byte-identical to dir2atr fixtures")
{
    for (const char* name : {"small", "n720", "n727", "n1199", "n3575", "n4367"}) {
        CAPTURE(name);
        const Golden g = load_golden(name);
        auto bytes = synth(g.size);
        DiskResult mr{};
        auto img = mount_bytes(bytes, &mr);
        REQUIRE(img);
        REQUIRE(img->geometry().sectorCount == g.sectors);
        CHECK(img->read_only());

        std::uint64_t h = 0xCBF29CE484222325ull;
        for (std::uint32_t s = 1; s <= g.sectors; ++s) {
            const auto sec = read_1based(*img, s);
            h = fnv1a(h, sec.data(), sec.size());
            const auto it = g.sector.find(s);
            if (it != g.sector.end()) {
                CAPTURE(s);
                CHECK(sec == it->second);
            }
        }
        CHECK(h == g.fnv);
    }
}

TEST_CASE("xex boot: boot sectors are the PicoBoot blob, directory and chain are as MyDOS expects")
{
    auto bytes = synth(1012);
    auto img = mount_bytes(bytes);
    REQUIRE(img);
    for (std::uint32_t s = 1; s <= 3; ++s) {
        const auto sec = read_1based(*img, s);
        CHECK(sec.size() == 128);
    }
    const auto s1 = read_1based(*img, 1);
    CHECK(std::memcmp(s1.data(), kPicoBootHead, sizeof(kPicoBootHead)) == 0);

    const auto dir = read_1based(*img, 361);
    CHECK(dir[0] == 0x42);
    CHECK(dir[1] == 4);  // 1012 bytes = 4 sectors of 253
    CHECK(dir[3] == 4);  // starts at sector 4
    CHECK(std::memcmp(&dir[5], "AUTORUN XEX", 11) == 0);
    CHECK(dir[16] == 0);

    const auto vtoc = read_1based(*img, 360);
    CHECK(vtoc[0] == 2);
    CHECK(vtoc[1] + (vtoc[2] << 8) == 708);
    CHECK(vtoc[3] + (vtoc[4] << 8) == 704);

    // Chain 4 -> 5 -> 6 -> 7 -> end; last sector's count is the remainder.
    for (std::uint32_t s = 4; s <= 7; ++s) {
        const auto d = read_1based(*img, s);
        const std::uint32_t next = (d[253] << 8) | d[254];
        CAPTURE(s);
        CHECK(next == (s < 7 ? s + 1 : 0));
        CHECK(d[255] == (s < 7 ? 253 : 1012 - 3 * 253));
        CHECK(std::memcmp(d.data(), bytes.data() + (s - 4) * 253, d[255]) == 0);
    }
    // Beyond the file: free sector.
    const auto free8 = read_1based(*img, 8);
    CHECK(free8 == std::vector<std::uint8_t>(256, 0));
}

TEST_CASE("xex boot: chain skips the directory and follows through 360-368")
{
    auto bytes = synth(1186 * 253);
    auto img = mount_bytes(bytes);
    REQUIRE(img);
    const auto d359 = read_1based(*img, 359);
    CHECK(((d359[253] << 8) | d359[254]) == 369);
    const auto last = read_1based(*img, 1198);
    CHECK(last[255] == 253);
    CHECK(last[253] == 0);
    CHECK(last[254] == 0);
    const auto dir = read_1based(*img, 361);
    CHECK(dir[0] == 0x46);
}

TEST_CASE("xex boot: read past end, writes rejected, unmount and remount")
{
    auto bytes = synth(5000);
    auto img = mount_bytes(bytes);
    REQUIRE(img);
    std::uint8_t buf[256];
    CHECK(img->read_sector(720, buf, sizeof(buf)).error == DiskError::OutOfRange);
    CHECK(img->read_sector(0, buf, 100).error == DiskError::InvalidSlot);
    CHECK(img->write_sector(5, buf, sizeof(buf)).error == DiskError::ReadOnly);

    REQUIRE(img->unmount().ok());
    CHECK(img->read_sector(0, buf, sizeof(buf)).error == DiskError::NotMounted);
    auto bytes2 = synth(300000);
    const auto r = img->mount(std::make_unique<MemoryFile>(bytes2, true, nullptr, nullptr), bytes2.size(), MountOptions{});
    REQUIRE(r.ok());
    CHECK(img->geometry().sectorCount == 1199);
    CHECK(img->read_sector(1198, buf, sizeof(buf)).ok());
    CHECK(img->read_sector(1199, buf, sizeof(buf)).error == DiskError::OutOfRange);
}

TEST_CASE("xex boot: rejects empty and non-executable files")
{
    std::vector<std::uint8_t> empty;
    DiskResult r{};
    CHECK(!mount_bytes(empty, &r));
    CHECK(r.error == DiskError::BadImage);
    std::vector<std::uint8_t> text(100, 'x');
    CHECK(!mount_bytes(text, &r));
    CHECK(r.error == DiskError::BadImage);
}

TEST_CASE("xex boot: DiskService mounts an xex read-write request as a read-only boot disk")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* mem = owned.get();
    mem->file_bytes("/games/prog.xex") = synth(300059);
    REQUIRE(storage.registerFileSystem(std::move(owned)));

    DiskService svc(storage, make_default_image_registry());
    MountOptions opts{};
    opts.readOnlyRequested = false;
    REQUIRE(svc.mount(0, "mem", "/games/prog.xex", opts).ok());
    const auto info = svc.info(0);
    CHECK(info.type == ImageType::XexBoot);
    CHECK(info.readOnly);
    CHECK(info.geometry.sectorSize == 256);
    CHECK(info.geometry.sectorCount == 1199);

    std::uint8_t sec[256]{};
    const auto r = svc.read_sector(0, 0, sec, sizeof(sec));
    REQUIRE(r.ok());
    CHECK(r.bytes == 128);
    CHECK(std::memcmp(sec, kPicoBootHead, sizeof(kPicoBootHead)) == 0);
    CHECK(svc.write_sector(0, 10, sec, sizeof(sec)).error == DiskError::ReadOnly);
    CHECK(mem->file_bytes("/games/prog.xex") == synth(300059)); // source untouched
}

TEST_CASE("xex boot: DiskDevice protocol mounts an xex and reads boot, directory and data sectors")
{
    namespace diskproto = fujinet::io::diskproto;
    using fujinet::io::IORequest;
    fujinet::fs::StorageManager sm;
    auto memfs = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    memfs->file_bytes("/prog.xex") = synth(1012);
    REQUIRE(sm.registerFileSystem(std::move(memfs)));
    fujinet::io::DiskDevice dev(sm);
    const auto deviceId = fujinet::io::protocol::to_device_id(fujinet::io::protocol::WireDeviceId::DiskService);

    {
        std::string p;
        diskproto::write_u8(p, 1);
        diskproto::write_u8(p, 1);
        diskproto::write_u8(p, 0); // rw requested
        diskproto::write_u8(p, 0); // auto
        diskproto::write_u16le(p, 0);
        diskproto::write_lp_u16_string(p, "mem:///prog.xex");
        IORequest req{};
        req.id = 1;
        req.deviceId = deviceId;
        req.command = 0x01;
        req.payload.assign(p.begin(), p.end());
        const auto resp = dev.handle(req);
        REQUIRE(resp.status == fujinet::io::StatusCode::Ok);
        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0, type = 0;
        std::uint16_t reserved = 0, ss = 0;
        std::uint32_t count = 0;
        REQUIRE(r.read_u8(ver)); REQUIRE(r.read_u8(flags)); REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot)); REQUIRE(r.read_u8(type)); REQUIRE(r.read_u16le(ss)); REQUIRE(r.read_u32le(count));
        CHECK((flags & 0x01) != 0);
        CHECK((flags & 0x02) != 0); // read-only reported
        CHECK(type == 6);
        CHECK(ss == 256);
        CHECK(count == 720);
    }

    auto read = [&](std::uint32_t lba, std::vector<std::uint8_t>& out) {
        std::string p;
        diskproto::write_u8(p, 1);
        diskproto::write_u8(p, 1);
        diskproto::write_u32le(p, lba);
        diskproto::write_u16le(p, 256);
        IORequest req{};
        req.id = 2;
        req.deviceId = deviceId;
        req.command = 0x03;
        req.payload.assign(p.begin(), p.end());
        const auto resp = dev.handle(req);
        REQUIRE(resp.status == fujinet::io::StatusCode::Ok);
        diskproto::Reader r(resp.payload.data(), resp.payload.size());
        std::uint8_t ver = 0, flags = 0, slot = 0;
        std::uint16_t reserved = 0, len = 0;
        std::uint32_t l = 0;
        const std::uint8_t* b = nullptr;
        REQUIRE(r.read_u8(ver)); REQUIRE(r.read_u8(flags)); REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u8(slot)); REQUIRE(r.read_u32le(l)); REQUIRE(r.read_u16le(len)); REQUIRE(r.read_bytes(b, len));
        out.assign(b, b + len);
    };

    std::vector<std::uint8_t> s;
    read(0, s);
    CHECK(s.size() == 128);
    CHECK(std::memcmp(s.data(), kPicoBootHead, sizeof(kPicoBootHead)) == 0);
    read(359, s);
    CHECK(s.size() == 256);
    CHECK(s[0] == 2);
    read(360, s);
    CHECK(std::memcmp(&s[5], "AUTORUN XEX", 11) == 0);
    read(3, s);
    const auto src = synth(1012);
    CHECK(std::memcmp(s.data(), src.data(), 253) == 0);
    CHECK(s[254] == 5);
}
