#include "doctest.h"

#include "fujinet/fs/fs_stdio.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {

std::vector<std::uint8_t> read_back(const std::filesystem::path& p)
{
    std::vector<std::uint8_t> out(std::filesystem::file_size(p));
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    REQUIRE(f);
    REQUIRE(std::fread(out.data(), 1, out.size(), f) == out.size());
    std::fclose(f);
    return out;
}

} // namespace

TEST_CASE("StdioFile: a write right after a read lands at the logical position")
{
    const auto dir = std::filesystem::temp_directory_path() / "fnio_fs_stdio_test";
    std::filesystem::create_directories(dir);
    {
        std::FILE* f = std::fopen((dir / "img.bin").string().c_str(), "wb");
        REQUIRE(f);
        std::vector<std::uint8_t> zeros(16 * 512, 0);
        std::fwrite(zeros.data(), 1, zeros.size(), f);
        std::fclose(f);
    }

    auto fs = fujinet::fs::create_stdio_filesystem(dir.string(), "t", fujinet::fs::FileSystemKind::HostPosix);
    REQUIRE(fs);
    {
        auto file = fs->open("/img.bin", "r+b");
        REQUIRE(file);
        std::uint8_t block[512];
        REQUIRE(file->seek(0));
        REQUIRE(file->read(block, sizeof(block)) == sizeof(block)); // stdio buffers ahead
        std::vector<std::uint8_t> ones(512, 0x11);
        REQUIRE(file->write(ones.data(), ones.size()) == ones.size()); // block 1
        REQUIRE(file->read(block, sizeof(block)) == sizeof(block));    // block 2
        CHECK(block[0] == 0);
        std::vector<std::uint8_t> twos(512, 0x22);
        REQUIRE(file->write(twos.data(), twos.size()) == twos.size()); // block 3
        REQUIRE(file->flush());
    }

    const auto bytes = read_back(dir / "img.bin");
    CHECK(bytes[0 * 512] == 0);
    CHECK(bytes[1 * 512] == 0x11);
    CHECK(bytes[2 * 512] == 0);
    CHECK(bytes[3 * 512] == 0x22);
    CHECK(bytes[8 * 512] == 0);
    std::filesystem::remove_all(dir);
}
