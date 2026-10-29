#include "fujinet/tnfs/tnfs_protocol.h"

#include <algorithm>
#include <chrono>

namespace fujinet::tnfs {

namespace {

char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool name_less(const std::string& a, const std::string& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(lower_ascii(a[i]));
        const unsigned char y = static_cast<unsigned char>(lower_ascii(b[i]));
        if (x != y) return x < y;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

std::string join(const std::string& dir, const std::string& name)
{
    if (dir.empty() || dir == "/") return "/" + name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

} // namespace

// TNFS 1.0 fallback: names via OPENDIR/READDIR, stat only the requested range.
TnfsListStatus ITnfsClient::listDirectoryRange(const std::string& path, fs::DirSort sort, std::size_t start,
                                               std::size_t count, std::vector<TnfsDirEntry>& out,
                                               std::size_t& total, std::chrono::milliseconds deadline)
{
    const auto limit = std::chrono::steady_clock::now() + deadline;
    out.clear();
    total = 0;

    std::vector<std::string> names = listDirectory(path);
    if (names.empty() && !isDirectory(path)) {
        return std::chrono::steady_clock::now() >= limit ? TnfsListStatus::Timeout : TnfsListStatus::Error;
    }
    if (sort != fs::DirSort::None) {
        std::sort(names.begin(), names.end(), name_less);
    }
    total = names.size();
    if (start >= total) {
        return TnfsListStatus::Ok;
    }
    const std::size_t end = (count > total - start) ? total : start + count;
    for (std::size_t i = start; i < end; ++i) {
        if (std::chrono::steady_clock::now() >= limit) {
            out.clear();
            return TnfsListStatus::Timeout;
        }
        TnfsDirEntry e;
        e.name = names[i];
        TnfsStat st{};
        if (stat(join(path, e.name), st)) {
            e.isDir = st.isDir;
            e.size = st.filesize;
            e.mTime = st.mTime;
        } else if (std::chrono::steady_clock::now() >= limit) {
            out.clear();
            return TnfsListStatus::Timeout;
        }
        out.push_back(std::move(e));
    }
    return TnfsListStatus::Ok;
}

} // namespace fujinet::tnfs
