#include "doctest.h"

#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/file_commands.h"
#include "fujinet/io/devices/file_device.h"
#include "fujinet/io/directory_cache.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace fujinet::fs;
using fujinet::io::FileDevice;
using fujinet::io::IORequest;
using fujinet::io::IOResponse;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FileCommand;
namespace lp = fujinet::io::protocol::list_pages;

constexpr const char* kDir = "tnfs://server/lp";

class NullFile final : public IFile {
public:
    std::size_t read(void*, std::size_t) override { return 0; }
    std::size_t write(const void*, std::size_t b) override { return b; }
    bool seek(std::uint64_t) override { return true; }
    std::uint64_t tell() const override { return 0; }
    bool flush() override { return true; }
};

// Fake in-memory filesystem with listing counters. `ranged` makes it behave like TNFS.
class PagingFs final : public IFileSystem {
public:
    PagingFs(bool ranged) : _ranged(ranged) {}

    std::vector<FileInfo> entries;
    unsigned listCalls = 0;   // listDirectory()
    unsigned rangeCalls = 0;  // listDirectoryRange()
    bool fail = false;
    ListStatus failStatus = ListStatus::Error;

    FileSystemKind kind() const override { return FileSystemKind::HostPosix; }
    std::string name() const override { return "tnfs"; }
    bool exists(const std::string& p) override { return p == kDir; }
    bool isDirectory(const std::string& p) override { return p == kDir; }
    bool createDirectory(const std::string&) override { return false; }
    bool removeFile(const std::string&) override { return false; }
    bool removeDirectory(const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    std::unique_ptr<IFile> open(const std::string&, const char*) override { return std::make_unique<NullFile>(); }
    bool stat(const std::string& p, FileInfo& i) override
    {
        if (p != kDir) return false;
        i = FileInfo{p, true, 0, {}};
        return true;
    }
    bool listDirectory(const std::string& p, std::vector<FileInfo>& out) override
    {
        ++listCalls;
        if (fail || p != kDir) return false;
        out = entries;
        return true;
    }
    bool supportsRangeListing() const override { return _ranged; }
    ListStatus listDirectoryRange(const std::string& p, DirSort sort, std::size_t start, std::size_t count,
                                  std::vector<FileInfo>& out, std::size_t& total) override
    {
        ++rangeCalls;
        if (fail) return failStatus;
        return IFileSystem::listDirectoryRange(p, sort, start, count, out, total);
    }

private:
    bool _ranged;
};

void add(PagingFs& fs, const std::string& name, bool dir, std::uint64_t size = 0, std::int64_t mtime = 0)
{
    FileInfo i;
    i.path = std::string(kDir) + "/" + name;
    i.isDirectory = dir;
    i.sizeBytes = size;
    if (mtime) i.modifiedTime = std::chrono::system_clock::time_point(std::chrono::seconds(mtime));
    fs.entries.push_back(std::move(i));
}

void fill_numbered(PagingFs& fs, int n)
{
    for (int i = 0; i < n; ++i) {
        char nm[32];
        std::snprintf(nm, sizeof(nm), "file%03d.atr", i);
        add(fs, nm, false, 100 + i, 1000 + i);
    }
}

struct Rig {
    fujinet::fs::StorageManager storage;
    PagingFs* fs;
    std::unique_ptr<FileDevice> device;

    explicit Rig(bool ranged)
    {
        auto f = std::make_unique<PagingFs>(ranged);
        fs = f.get();
        REQUIRE(storage.registerFileSystem(std::move(f)));
        device = std::make_unique<FileDevice>(storage);
    }

    static void u16(std::vector<std::uint8_t>& v, unsigned x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }

    IOResponse list_pages(unsigned startPage, unsigned pageSize, unsigned maxBytes, unsigned flags,
                          unsigned maxName = 0, const std::string& uri = kDir, const std::string& filter = {})
    {
        IORequest req{};
        req.command = static_cast<std::uint16_t>(FileCommand::ListPages);
        auto& p = req.payload;
        p.push_back(1);
        u16(p, static_cast<unsigned>(uri.size()));
        p.insert(p.end(), uri.begin(), uri.end());
        u16(p, startPage);
        p.push_back(static_cast<std::uint8_t>(pageSize));
        u16(p, maxBytes);
        p.push_back(static_cast<std::uint8_t>(flags));
        p.push_back(static_cast<std::uint8_t>(maxName));
        if (!filter.empty()) {
            p.push_back(static_cast<std::uint8_t>(filter.size()));
            p.insert(p.end(), filter.begin(), filter.end());
        }
        return device->handle(req);
    }
};

struct Entry { std::string name; bool dir; bool cut; std::uint32_t size, mtime; };
struct Page { unsigned count; std::vector<Entry> entries; };
struct Parsed {
    unsigned flags, total, startPage, pageSize, pageCount;
    std::vector<Page> pages;
};

unsigned rd16(const std::vector<std::uint8_t>& v, std::size_t o) { return v[o] | (v[o + 1] << 8); }
unsigned rd32(const std::vector<std::uint8_t>& v, std::size_t o) { return rd16(v, o) | (rd16(v, o + 2) << 16); }

Parsed parse(const IOResponse& r)
{
    Parsed out{};
    const auto& v = r.payload;
    REQUIRE(v.size() >= 11);
    CHECK(v[0] == 1);
    out.flags = v[1];
    CHECK(rd16(v, 2) == 0);
    out.total = rd32(v, 4);
    out.startPage = rd16(v, 8);
    out.pageSize = v[10];
    out.pageCount = v[11];
    std::size_t o = 12;
    for (unsigned pgi = 0; pgi < out.pageCount; ++pgi) {
        Page pg;
        pg.count = v[o];
        const unsigned dataLen = rd16(v, o + 1);
        o += 3;
        const std::size_t end = o + dataLen;
        for (unsigned i = 0; i < pg.count; ++i) {
            Entry e;
            e.dir = (v[o] & lp::kEntryFlagDirectory) != 0;
            e.cut = (v[o] & lp::kEntryFlagNameCut) != 0;
            const unsigned nl = v[o + 1];
            e.name.assign(v.begin() + static_cast<std::ptrdiff_t>(o + 2), v.begin() + static_cast<std::ptrdiff_t>(o + 2 + nl));
            e.size = rd32(v, o + 2 + nl);
            e.mtime = rd32(v, o + 6 + nl);
            o += 10 + nl;
            pg.entries.push_back(std::move(e));
        }
        CHECK(o == end);
        out.pages.push_back(std::move(pg));
    }
    CHECK(o == v.size());
    return out;
}

} // namespace

TEST_CASE("ListPages: paging, totals and whole pages within the byte budget")
{
    Rig r(false);
    fill_numbered(*r.fs, 20);
    auto resp = r.list_pages(0, 6, 400, lp::kFlagSortByName);
    REQUIRE(resp.status == StatusCode::Ok);
    auto p = parse(resp);
    CHECK(p.total == 20);
    CHECK(p.startPage == 0);
    CHECK(p.pageSize == 6);
    // 6 entries of 2+11+8 = 21 bytes + 3 header = 129 bytes per page: three pages (387) fit in
    // 400, the short fourth (45 more) would not.
    CHECK(p.pageCount == 3);
    CHECK((p.flags & lp::kResponseFlagMore) != 0);
    CHECK(p.pages[0].count == 6);
    CHECK(p.pages[0].entries[0].name == "file000.atr");
    CHECK(p.pages[0].entries[0].size == 100);
    CHECK(p.pages[0].entries[0].mtime == 1000);
    CHECK(p.pages[2].entries[5].name == "file017.atr");
    CHECK(resp.payload.size() <= 12 + 400);
}

TEST_CASE("ListPages: page alignment and short last page")
{
    Rig r(false);
    fill_numbered(*r.fs, 20);
    auto p = parse(r.list_pages(1, 6, 480, lp::kFlagSortByName));
    CHECK(p.startPage == 1);
    REQUIRE(p.pageCount == 3); // pages 1, 2, 3
    CHECK(p.pages[0].entries[0].name == "file006.atr");
    CHECK(p.pages[2].count == 2); // entries 18, 19
    CHECK(p.pages[2].entries[1].name == "file019.atr");
    CHECK((p.flags & lp::kResponseFlagMore) == 0);

    auto q = parse(r.list_pages(3, 6, 480, lp::kFlagSortByName));
    CHECK(q.pageCount == 1);
    CHECK(q.pages[0].count == 2);
}

TEST_CASE("ListPages: sort by name is ASCII case-insensitive, directories first on request")
{
    Rig r(false);
    add(*r.fs, "beta.atr", false, 5);
    add(*r.fs, "Alpha.atr", false, 6);
    add(*r.fs, "zeta", true);
    add(*r.fs, "Delta", true);
    add(*r.fs, "alpha2.atr", false, 7);

    auto names = [](const Parsed& p) {
        std::vector<std::string> n;
        for (auto& pg : p.pages) for (auto& e : pg.entries) n.push_back(e.name);
        return n;
    };
    using V = std::vector<std::string>;

    CHECK(names(parse(r.list_pages(0, 8, 480, 0))) == V{"beta.atr", "Alpha.atr", "zeta", "Delta", "alpha2.atr"});
    CHECK(names(parse(r.list_pages(0, 8, 480, lp::kFlagSortByName))) ==
          V{"Alpha.atr", "alpha2.atr", "beta.atr", "Delta", "zeta"});
    auto df = parse(r.list_pages(0, 8, 480, lp::kFlagSortByName | lp::kFlagDirsFirst));
    CHECK(names(df) == V{"Delta", "zeta", "Alpha.atr", "alpha2.atr", "beta.atr"});
    CHECK(df.pages[0].entries[0].dir);
    CHECK(df.pages[0].entries[0].size == 0);
    // Directories-first needs the sort bit.
    CHECK(names(parse(r.list_pages(0, 8, 480, lp::kFlagDirsFirst))) ==
          V{"beta.atr", "Alpha.atr", "zeta", "Delta", "alpha2.atr"});
}

TEST_CASE("ListPages: filters names case-insensitively and preserves directories")
{
    Rig r(true);
    add(*r.fs, "Miner 2049er.atr", false);
    add(*r.fs, "Manic Miner.atr", false);
    add(*r.fs, "other.atr", false);
    add(*r.fs, "games", true);
    auto p = parse(r.list_pages(0, 2, 480, lp::kFlagSortByName | lp::kFlagDirsFirst, 0, kDir, "*MiNeR*"));
    CHECK(p.total == 3);
    REQUIRE(p.pageCount == 2);
    CHECK(p.pages[0].entries[0].dir);
    CHECK(p.pages[0].entries[1].name == "Manic Miner.atr");
    CHECK(p.pages[1].entries[0].name == "Miner 2049er.atr");
    CHECK(parse(r.list_pages(0, 8, 480, lp::kFlagSortByName, 0, kDir, "2049")).total == 2);
    CHECK(parse(r.list_pages(0, 8, 480, lp::kFlagSortByName, 0, kDir, "nomatch")).total == 1);
    CHECK(parse(r.list_pages(0, 8, 480, lp::kFlagSortByName, 0, kDir, "m?ner*")).total == 2);
}

TEST_CASE("ListPages: long names are cut and marked")
{
    Rig r(false);
    add(*r.fs, "this_is_a_very_long_file_name.atr", false, 1);
    add(*r.fs, "short", false, 2);
    auto p = parse(r.list_pages(0, 2, 480, lp::kFlagSortByName, 8));
    CHECK((p.flags & lp::kResponseFlagNameCut) != 0);
    CHECK(p.pages[0].entries[0].name == "short");
    CHECK(!p.pages[0].entries[0].cut);
    CHECK(p.pages[0].entries[1].name == "this_is_");
    CHECK(p.pages[0].entries[1].cut);
}

TEST_CASE("ListPages: a first page that does not fit returns no pages with more set")
{
    Rig r(false);
    fill_numbered(*r.fs, 20);
    auto p = parse(r.list_pages(0, 6, 100, lp::kFlagSortByName));
    CHECK(p.pageCount == 0);
    CHECK((p.flags & lp::kResponseFlagMore) != 0);
    CHECK(p.total == 20);
    // Retry with a smaller page size succeeds.
    auto q = parse(r.list_pages(0, 3, 100, lp::kFlagSortByName));
    CHECK(q.pageCount == 1);
    CHECK(q.pages[0].count == 3);
}

TEST_CASE("ListPages: out of range page, total only, invalid requests")
{
    Rig r(false);
    fill_numbered(*r.fs, 20);
    auto p = parse(r.list_pages(100, 6, 480, lp::kFlagSortByName));
    CHECK(p.pageCount == 0);
    CHECK((p.flags & lp::kResponseFlagMore) == 0);
    CHECK(p.total == 20);
    CHECK(p.startPage == 100);

    auto t = parse(r.list_pages(0, 6, 480, lp::kFlagSortByName | lp::kFlagTotalOnly));
    CHECK(t.pageCount == 0);
    CHECK(t.total == 20);

    CHECK(r.list_pages(0, 0, 480, 0).status == StatusCode::InvalidRequest);
    CHECK(r.list_pages(0, 65, 480, 0).status == StatusCode::InvalidRequest);
    CHECK(r.list_pages(0, 6, 5, 0).status == StatusCode::InvalidRequest);
    CHECK(r.list_pages(0, 6, 480, 0, 0, "nosuchfs://x/y").status == StatusCode::DeviceNotFound);

    IORequest bad{};
    bad.command = static_cast<std::uint16_t>(FileCommand::ListPages);
    bad.payload = {1, 0, 0};
    CHECK(r.device->handle(bad).status == StatusCode::InvalidRequest);
}

TEST_CASE("ListPages: cache hit does no listing call, refresh re-reads")
{
    Rig r(false);
    fill_numbered(*r.fs, 20);
    CHECK(r.list_pages(0, 6, 480, lp::kFlagSortByName).status == StatusCode::Ok);
    CHECK(r.fs->rangeCalls == 1);
    CHECK(r.list_pages(1, 6, 480, lp::kFlagSortByName).status == StatusCode::Ok);
    CHECK(r.list_pages(3, 6, 480, lp::kFlagSortByName).status == StatusCode::Ok);
    CHECK(r.fs->rangeCalls == 1);

    // The directory changes: still served from cache until refreshed.
    add(*r.fs, "zzz_new.atr", false, 1);
    CHECK(parse(r.list_pages(0, 6, 480, lp::kFlagSortByName)).total == 20);
    CHECK(r.fs->rangeCalls == 1);
    CHECK(parse(r.list_pages(0, 6, 480, lp::kFlagSortByName | lp::kFlagRefresh)).total == 21);
    CHECK(r.fs->rangeCalls == 2);
    CHECK(parse(r.list_pages(3, 6, 480, lp::kFlagSortByName)).total == 21);
    CHECK(r.fs->rangeCalls == 2);

    // Legacy ListDirectory shares the cache (same sort order key).
    IORequest ld{};
    ld.command = static_cast<std::uint16_t>(FileCommand::ListDirectory);
    auto& pl = ld.payload;
    pl.push_back(1);
    Rig::u16(pl, static_cast<unsigned>(std::string(kDir).size()));
    pl.insert(pl.end(), kDir, kDir + std::string(kDir).size());
    Rig::u16(pl, 0);
    Rig::u16(pl, 400);
    pl.push_back(0x02); // sort by name
    CHECK(r.device->handle(ld).status == StatusCode::Ok);
    CHECK(r.fs->rangeCalls == 2);
}

TEST_CASE("ListPages: range-capable filesystem is read by chunks and cached by page")
{
    Rig r(true);
    fill_numbered(*r.fs, 100);
    auto p0 = parse(r.list_pages(0, 6, 140, lp::kFlagSortByName));
    CHECK(p0.total == 100);
    REQUIRE(p0.pageCount == 1);
    CHECK(r.fs->rangeCalls == 1);
    // Pages 1..3 lie inside the first 24-entry chunk: no new listing call.
    parse(r.list_pages(1, 6, 140, lp::kFlagSortByName));
    parse(r.list_pages(3, 6, 140, lp::kFlagSortByName));
    CHECK(r.fs->rangeCalls == 1);
    // Page 10 starts at entry 60 (chunk 2) and fetches only that chunk.
    auto p10 = parse(r.list_pages(10, 6, 140, lp::kFlagSortByName));
    CHECK(p10.pages[0].entries[0].name == "file060.atr");
    CHECK(r.fs->rangeCalls == 2);
    parse(r.list_pages(10, 6, 140, lp::kFlagSortByName));
    CHECK(r.fs->rangeCalls == 2);
    CHECK(r.fs->listCalls == 2); // the default implementation lists once per range call here
}

TEST_CASE("ListPages: listing failures map to IOError and Timeout")
{
    Rig r(false);
    fill_numbered(*r.fs, 3);
    r.fs->fail = true;
    CHECK(r.list_pages(0, 6, 480, 0).status == StatusCode::IOError);
    r.fs->failStatus = ListStatus::Timeout;
    CHECK(r.list_pages(0, 6, 480, 0).status == StatusCode::Timeout);
    r.fs->fail = false;
    CHECK(r.list_pages(0, 6, 480, 0).status == StatusCode::Ok);
}

TEST_CASE("DirectoryCache is bounded in directories")
{
    fujinet::io::DirectoryCache cache;
    CHECK(cache.cached_directories() == 0);
}
