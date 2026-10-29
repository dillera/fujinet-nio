#include "fujinet/io/directory_cache.h"

#include <algorithm>

namespace fujinet::io {

using fs::ListStatus;

DirectoryCache::Dir* DirectoryCache::find(const std::string& uri, fs::DirSort sort)
{
    for (auto& d : _dirs) {
        if (d.uri == uri && d.sort == sort) {
            return &d;
        }
    }
    return nullptr;
}

std::size_t DirectoryCache::cached_entries() const
{
    std::size_t n = 0;
    for (const auto& d : _dirs) {
        for (const auto& c : d.chunks) {
            n += c.second.size();
        }
    }
    return n;
}

void DirectoryCache::enforce_bounds(const std::string& keepUri, fs::DirSort keepSort, std::size_t keepChunk)
{
    // Whole directories first (least recently used), never the one in use.
    while (_dirs.size() > kMaxDirectories || cached_entries() > kMaxEntries) {
        auto victim = _dirs.end();
        for (auto it = _dirs.begin(); it != _dirs.end(); ++it) {
            if (it->uri == keepUri && it->sort == keepSort) continue;
            if (victim == _dirs.end() || it->lastUse < victim->lastUse) victim = it;
        }
        if (victim != _dirs.end()) {
            _dirs.erase(victim);
            continue;
        }
        // Only the directory in use is left: drop its chunks farthest from the one just used.
        Dir* d = find(keepUri, keepSort);
        if (d == nullptr || d->chunks.size() <= 1) break;
        auto far = d->chunks.end();
        std::size_t farDist = 0;
        for (auto it = d->chunks.begin(); it != d->chunks.end(); ++it) {
            if (it->first == keepChunk) continue;
            const std::size_t dist = it->first > keepChunk ? it->first - keepChunk : keepChunk - it->first;
            if (far == d->chunks.end() || dist > farDist) { far = it; farDist = dist; }
        }
        if (far == d->chunks.end()) break;
        d->chunks.erase(far);
    }
}

ListStatus DirectoryCache::get_range(fs::IFileSystem& fs, const std::string& canonicalUri,
                                     const std::string& fsPath, fs::DirSort sort, bool refresh,
                                     std::size_t start, std::size_t count,
                                     std::vector<fs::FileInfo>& out, std::size_t& total)
{
    out.clear();
    total = 0;
    const auto now = std::chrono::steady_clock::now();

    // Drop expired listings and a refreshed one.
    for (auto it = _dirs.begin(); it != _dirs.end();) {
        const bool mine = (it->uri == canonicalUri && it->sort == sort);
        if (now > it->expires || (refresh && mine)) {
            it = _dirs.erase(it);
        } else {
            ++it;
        }
    }

    const bool ranged = fs.supportsRangeListing();

    if (!ranged) {
        Dir* d = find(canonicalUri, sort);
        if (d == nullptr) {
            std::vector<fs::FileInfo> all;
            std::size_t t = 0;
            const ListStatus st = fs.listDirectoryRange(fsPath, sort, 0, static_cast<std::size_t>(-1), all, t);
            if (st != ListStatus::Ok) {
                return st;
            }
            total = all.size();
            if (start < total) {
                const std::size_t end = (count > total - start) ? total : start + count;
                out.assign(all.begin() + static_cast<std::ptrdiff_t>(start),
                           all.begin() + static_cast<std::ptrdiff_t>(end));
            }
            if (all.size() <= kMaxEntries) {
                Dir nd;
                nd.uri = canonicalUri;
                nd.sort = sort;
                nd.ranged = false;
                nd.totalKnown = true;
                nd.total = total;
                nd.chunks[0] = std::move(all);
                nd.expires = now + kTtl;
                nd.lastUse = ++_tick;
                _dirs.push_back(std::move(nd));
                enforce_bounds(canonicalUri, sort, 0);
            }
            return ListStatus::Ok;
        }
        d->expires = now + kTtl;
        d->lastUse = ++_tick;
        total = d->total;
        const auto& all = d->chunks[0];
        if (start < total) {
            const std::size_t end = (count > total - start) ? total : start + count;
            out.assign(all.begin() + static_cast<std::ptrdiff_t>(start),
                       all.begin() + static_cast<std::ptrdiff_t>(end));
        }
        return ListStatus::Ok;
    }

    // Ranged: fetch the aligned chunks covering [start, start+count) that are not cached.
    Dir* d = find(canonicalUri, sort);
    if (d == nullptr) {
        Dir nd;
        nd.uri = canonicalUri;
        nd.sort = sort;
        nd.ranged = true;
        _dirs.push_back(std::move(nd));
        d = &_dirs.back();
    }
    d->expires = now + kTtl;
    d->lastUse = ++_tick;

    const std::size_t kChunk = kChunkEntries;
    std::size_t pos = start;
    std::size_t end = (count > static_cast<std::size_t>(-1) - start) ? static_cast<std::size_t>(-1) : start + count;
    while (pos < end) {
        if (d->totalKnown && pos >= d->total) {
            break;
        }
        const std::size_t ci = pos / kChunk;
        auto it = d->chunks.find(ci);
        if (it == d->chunks.end()) {
            std::vector<fs::FileInfo> chunk;
            std::size_t t = 0;
            const ListStatus st = fs.listDirectoryRange(fsPath, sort, ci * kChunk, kChunk, chunk, t);
            if (st != ListStatus::Ok) {
                // Keep what we have only if the directory was already known; drop an unknown one.
                if (!d->totalKnown) {
                    _dirs.erase(_dirs.begin() + (d - _dirs.data()));
                }
                out.clear();
                return st;
            }
            if (d->totalKnown && d->total != t) {
                d->chunks.clear(); // directory changed underneath us
            }
            d->totalKnown = true;
            d->total = t;
            it = d->chunks.emplace(ci, std::move(chunk)).first;
            enforce_bounds(canonicalUri, sort, ci);
            d = find(canonicalUri, sort);
            if (d == nullptr) {
                out.clear();
                return ListStatus::Error;
            }
            it = d->chunks.find(ci);
            if (it == d->chunks.end()) {
                // Single chunk larger than the bound cannot happen (kChunk < kMaxEntries).
                out.clear();
                return ListStatus::Error;
            }
            if (pos >= d->total) {
                break;
            }
        }
        const auto& chunk = it->second;
        const std::size_t off = pos - ci * kChunk;
        if (off >= chunk.size()) {
            break; // short chunk: directory ended
        }
        const std::size_t take = std::min(chunk.size() - off, end - pos);
        out.insert(out.end(), chunk.begin() + static_cast<std::ptrdiff_t>(off),
                   chunk.begin() + static_cast<std::ptrdiff_t>(off + take));
        pos += take;
    }
    total = d->totalKnown ? d->total : 0;
    return ListStatus::Ok;
}

} // namespace fujinet::io
