#include "doctest.h"

#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fujinet::fs;
using namespace fujinet::tnfs;

namespace {

// Counts requests and enforces an open-file limit.
class FakeTnfsServer : public ITnfsClient {
public:
    explicit FakeTnfsServer(bool parallel) : _parallel(parallel) {}

    std::map<std::string, std::vector<std::uint8_t>> files;
    std::size_t maxOpen = 16;
    int reads = 0, writes = 0, seeks = 0, batches = 0;

    bool mount(const std::string&, const std::string&, const std::string&) override { return true; }
    bool umount() override { return true; }
    bool stat(const std::string& path, TnfsStat& st) override
    {
        auto it = files.find(path);
        if (it == files.end()) return false;
        st = {};
        st.filesize = static_cast<uint32_t>(it->second.size());
        return true;
    }
    bool exists(const std::string& path) override { return files.count(path) != 0; }
    bool isDirectory(const std::string&) override { return false; }
    bool createDirectory(const std::string&) override { return true; }
    bool removeDirectory(const std::string&) override { return true; }
    bool removeFile(const std::string&) override { return true; }
    bool rename(const std::string&, const std::string&) override { return true; }
    std::vector<std::string> listDirectory(const std::string&) override { return {}; }

    int open(const std::string& path, uint16_t, uint16_t) override
    {
        if (!files.count(path) || _handles.size() >= maxOpen) return -1;
        const int h = _next++;
        _handles[h] = {path, 0};
        return h;
    }
    bool close(int h) override { return _handles.erase(h) != 0; }

    std::size_t read(int h, void* buf, std::size_t bytes) override
    {
        ++reads;
        return do_read(h, buf, std::min<std::size_t>(bytes, 512));
    }
    std::size_t write(int h, const void* buf, std::size_t bytes) override
    {
        ++writes;
        auto& hd = _handles.at(h);
        auto& f = files[hd.path];
        bytes = std::min<std::size_t>(bytes, 512);
        if (hd.pos + bytes > f.size()) f.resize(hd.pos + bytes);
        std::memcpy(f.data() + hd.pos, buf, bytes);
        hd.pos += bytes;
        return bytes;
    }
    bool seek(int h, uint32_t offset) override
    {
        ++seeks;
        _handles.at(h).pos = offset;
        return true;
    }
    uint32_t tell(int h) override { return _handles.at(h).pos; }

    bool supports_parallel_reads() const override { return _parallel; }
    void read_parallel(std::vector<ReadAt>& batch) override
    {
        ++batches;
        for (auto& r : batch) {
            _handles.at(r.fileHandle).pos = r.offset;
            r.got = do_read(r.fileHandle, r.dst, r.bytes);
        }
    }

    std::size_t open_handles() const { return _handles.size(); }

private:
    struct Handle {
        std::string path;
        uint32_t pos;
    };

    std::size_t do_read(int h, void* buf, std::size_t bytes)
    {
        auto& hd = _handles.at(h);
        const auto& f = files[hd.path];
        if (hd.pos >= f.size()) return 0;
        const std::size_t n = std::min<std::size_t>(bytes, f.size() - hd.pos);
        std::memcpy(buf, f.data() + hd.pos, n);
        hd.pos += static_cast<uint32_t>(n);
        return n;
    }

    bool _parallel;
    int _next = 1;
    std::map<int, Handle> _handles;
};

std::vector<std::uint8_t> image(std::size_t blocks)
{
    std::vector<std::uint8_t> v(blocks * 512);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(i / 512 * 3 + i);
    return v;
}

std::vector<std::uint8_t> read_block(IFile& f, std::size_t block)
{
    std::vector<std::uint8_t> out(512);
    REQUIRE(f.seek(block * 512));
    REQUIRE(f.read(out.data(), out.size()) == 512);
    return out;
}

std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& v, std::size_t block)
{
    return {v.begin() + block * 512, v.begin() + (block + 1) * 512};
}

} // namespace

TEST_CASE("TnfsFile: a missed block is fetched with the blocks after it")
{
    auto server = std::make_shared<FakeTnfsServer>(true);
    server->files["/img.hda"] = image(64);
    auto fs = make_tnfs_filesystem(server);
    auto f = fs->open("/img.hda", "r+b");
    REQUIRE(f);

    for (std::size_t b = 0; b < 16; ++b) {
        CHECK(read_block(*f, b) == slice(server->files["/img.hda"], b));
    }
    CHECK(server->batches == 2); // blocks 0-7, then 8-15
    CHECK(server->reads == 0);
    CHECK(server->open_handles() == 8);

    // Cached blocks cost no request at all, seeks included.
    const int seeks = server->seeks;
    CHECK(read_block(*f, 3) == slice(server->files["/img.hda"], 3));
    CHECK(server->seeks == seeks);
    CHECK(server->batches == 2);

    // The end of the file: only the blocks that exist are cached.
    CHECK(read_block(*f, 62) == slice(server->files["/img.hda"], 62));
    CHECK(read_block(*f, 63) == slice(server->files["/img.hda"], 63));
    std::uint8_t past[512];
    REQUIRE(f->seek(64 * 512));
    CHECK(f->read(past, sizeof(past)) == 0);
}

TEST_CASE("TnfsFile: writes reach the server and keep the cache in step")
{
    auto server = std::make_shared<FakeTnfsServer>(true);
    server->files["/img.hda"] = image(32);
    auto fs = make_tnfs_filesystem(server);
    auto f = fs->open("/img.hda", "r+b");
    REQUIRE(f);

    read_block(*f, 0); // caches 0-7
    std::vector<std::uint8_t> ones(512, 0x11);
    REQUIRE(f->seek(2 * 512));
    REQUIRE(f->write(ones.data(), ones.size()) == 512);
    CHECK(slice(server->files["/img.hda"], 2) == ones);
    CHECK(read_block(*f, 2) == ones);

    // A write that covers part of a cached block drops that block.
    std::vector<std::uint8_t> bit(100, 0x22);
    REQUIRE(f->seek(4 * 512 + 10));
    REQUIRE(f->write(bit.data(), bit.size()) == bit.size());
    const auto b4 = read_block(*f, 4);
    CHECK(b4 == slice(server->files["/img.hda"], 4));
    CHECK(b4[10] == 0x22);

    // A sequential write after a cached read lands at the logical position.
    read_block(*f, 5);
    std::vector<std::uint8_t> threes(512, 0x33);
    REQUIRE(f->write(threes.data(), threes.size()) == 512);
    CHECK(slice(server->files["/img.hda"], 6) == threes);
    CHECK(read_block(*f, 6) == threes);
}

TEST_CASE("TnfsFile: read-ahead handles are given back when the server runs out")
{
    auto server = std::make_shared<FakeTnfsServer>(true);
    server->files["/a.hda"] = image(16);
    server->files["/b.hda"] = image(16);
    server->files["/c.hda"] = image(16);
    auto fs = make_tnfs_filesystem(server);

    auto a = fs->open("/a.hda", "r+b");
    auto b = fs->open("/b.hda", "r+b");
    REQUIRE(a);
    REQUIRE(b);
    read_block(*a, 0);
    read_block(*b, 0);
    CHECK(server->open_handles() == 16);

    auto c = fs->open("/c.hda", "r+b");
    REQUIRE(c);
    CHECK(read_block(*c, 9) == slice(server->files["/c.hda"], 9));
    CHECK(read_block(*a, 12) == slice(server->files["/a.hda"], 12));
    CHECK(server->open_handles() <= 16);

    a.reset();
    b.reset();
    c.reset();
    CHECK(server->open_handles() == 0);
}

TEST_CASE("TnfsFile: without parallel reads, blocks are read one by one and cached")
{
    auto server = std::make_shared<FakeTnfsServer>(false);
    server->files["/img.hda"] = image(8);
    auto fs = make_tnfs_filesystem(server);
    auto f = fs->open("/img.hda", "rb");
    REQUIRE(f);

    for (std::size_t b = 0; b < 8; ++b) {
        CHECK(read_block(*f, b) == slice(server->files["/img.hda"], b));
    }
    CHECK(server->reads == 8);
    CHECK(server->seeks == 0); // sequential from the start: no seek needed
    CHECK(server->open_handles() == 1);
    CHECK(read_block(*f, 5) == slice(server->files["/img.hda"], 5));
    CHECK(server->reads == 8);
}
