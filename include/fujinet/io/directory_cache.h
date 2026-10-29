#pragma once

#include "fujinet/fs/filesystem.h"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace fujinet::io {

// Small bounded cache of sorted directory listings, shared by FileDevice ListDirectory and
// ListPages. Key: canonical uri + sort order. Filesystems that can page cheaply (TNFS) are
// cached by aligned chunks plus the directory total, so a page that was already fetched never
// touches the network; other filesystems are listed once, sorted, and held whole.
class DirectoryCache {
public:
    static constexpr std::chrono::seconds kTtl{120};
    static constexpr std::size_t kMaxDirectories = 4;
    static constexpr std::size_t kMaxEntries = 1024; // across all cached directories
    static constexpr std::size_t kChunkEntries = 24;

    // Entries [start, start+count) (clamped to the directory) into `out`, and the total.
    // `refresh` drops any cached listing for the key first. Status Timeout/Error on failure.
    fs::ListStatus get_range(fs::IFileSystem& fs, const std::string& canonicalUri,
                             const std::string& fsPath, fs::DirSort sort, bool refresh,
                             std::size_t start, std::size_t count,
                             std::vector<fs::FileInfo>& out, std::size_t& total);

    std::size_t cached_entries() const;
    std::size_t cached_directories() const { return _dirs.size(); }
    void clear() { _dirs.clear(); }

private:
    struct Dir {
        std::string uri;
        fs::DirSort sort{fs::DirSort::None};
        bool ranged{false};
        bool totalKnown{false};
        std::size_t total{0};
        std::map<std::size_t, std::vector<fs::FileInfo>> chunks; // chunk index -> entries
        std::chrono::steady_clock::time_point expires{};
        std::uint64_t lastUse{0};
    };

    Dir* find(const std::string& uri, fs::DirSort sort);
    void enforce_bounds(const std::string& keepUri, fs::DirSort keepSort, std::size_t keepChunk);

    std::vector<Dir> _dirs;
    std::uint64_t _tick{0};
};

} // namespace fujinet::io
