#include "fujinet/fs/filesystem.h"

#include <algorithm>
#include <string_view>

namespace fujinet::fs {

namespace {

std::string_view basename_of(const std::string& s)
{
    const auto pos = s.find_last_of('/');
    if (pos == std::string::npos || pos + 1 >= s.size()) {
        return std::string_view{s};
    }
    return std::string_view{s}.substr(pos + 1);
}

char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

int compare_ci(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = lower_ascii(a[i]);
        const char y = lower_ascii(b[i]);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
        }
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

} // namespace

bool name_less_ascii_ci(const std::string& a, const std::string& b)
{
    const int c = compare_ci(a, b);
    return c != 0 ? c < 0 : a < b;
}

void sort_directory_entries(std::vector<FileInfo>& entries, DirSort sort)
{
    if (sort == DirSort::None) {
        return;
    }
    const bool dirsFirst = (sort == DirSort::NameDirsFirst);
    std::stable_sort(entries.begin(), entries.end(), [dirsFirst](const FileInfo& a, const FileInfo& b) {
        if (dirsFirst && a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }
        const std::string_view na = basename_of(a.path);
        const std::string_view nb = basename_of(b.path);
        const int c = compare_ci(na, nb);
        return c != 0 ? c < 0 : na < nb;
    });
}

ListStatus IFileSystem::listDirectoryRange(const std::string& path, DirSort sort, std::size_t start,
                                           std::size_t count, std::vector<FileInfo>& out, std::size_t& total)
{
    out.clear();
    total = 0;
    std::vector<FileInfo> all;
    if (!listDirectory(path, all)) {
        return ListStatus::Error;
    }
    sort_directory_entries(all, sort);
    total = all.size();
    if (start >= total) {
        return ListStatus::Ok;
    }
    const std::size_t end = (count > total - start) ? total : start + count;
    if (start == 0 && end == total) {
        out = std::move(all);
    } else {
        out.assign(std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(start)),
                   std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(end)));
    }
    return ListStatus::Ok;
}

} // namespace fujinet::fs
