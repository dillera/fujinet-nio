
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/fs/uri_parser.h"
#include "fujinet/core/logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace fujinet::fs {

static constexpr const char* TAG = "tnfs_fs";

class TnfsFile;

// Lets a failed open reclaim read-ahead handles (tnfsd allows 16 per session).
struct TnfsOpenFiles {
    std::vector<TnfsFile*> files;
};

// Every TNFS request is a round trip, so blocks are cached and read ahead in
// parallel over extra handles. Another client's writes are not seen while cached.
class TnfsFile final : public IFile {
public:
    static constexpr std::size_t kBlock = 512;
    static constexpr std::size_t kCacheBlocks = 64;  // 32 KB, allocated on first block read
    static constexpr std::size_t kReadAhead = 8;     // blocks per parallel fetch
    static constexpr std::uint64_t kUnknown = ~std::uint64_t{0};

    TnfsFile(std::shared_ptr<tnfs::ITnfsClient> client, int fileHandle, std::string path, bool cacheable,
             std::shared_ptr<TnfsOpenFiles> openFiles)
        : _client(std::move(client))
        , _fileHandle(fileHandle)
        , _path(std::move(path))
        , _cacheable(cacheable)
        , _openFiles(std::move(openFiles))
    {
        _openFiles->files.push_back(this);
        FN_LOGD(TAG, "File handle %d created", fileHandle);
    }

    ~TnfsFile() override {
        auto& files = _openFiles->files;
        files.erase(std::remove(files.begin(), files.end(), this), files.end());
        drop_lanes();
        if (_fileHandle != -1) {
            _client->close(_fileHandle);
            FN_LOGD(TAG, "File handle %d closed", _fileHandle);
        }
    }

    std::size_t read(void* dst, std::size_t maxBytes) override {
        auto* out = static_cast<std::uint8_t*>(dst);
        if (_cacheable && maxBytes == kBlock && _position % kBlock == 0) {
            if (cache_get(_position, out) || (read_ahead(_position) && cache_get(_position, out))) {
                _position += kBlock;
                return kBlock;
            }
        }

        if (!sync_position()) {
            return 0;
        }
        const std::size_t bytesRead = _client->read(_fileHandle, out, maxBytes);
        if (bytesRead == std::min<std::size_t>(maxBytes, kBlock)) {
            _serverPos += bytesRead;
        } else {
            _serverPos = kUnknown;
        }
        if (_cacheable && bytesRead == kBlock && _position % kBlock == 0) {
            cache_put(_position, out);
        }
        _position += bytesRead;
        return bytesRead;
    }

    std::size_t write(const void* src, std::size_t bytes) override {
        const auto* in = static_cast<const std::uint8_t*>(src);
        if (!sync_position()) {
            return 0;
        }
        const std::size_t bytesWritten = _client->write(_fileHandle, in, bytes);
        if (bytesWritten == std::min<std::size_t>(bytes, kBlock)) {
            _serverPos += bytesWritten;
        } else {
            _serverPos = kUnknown;
        }
        cache_written(_position, in, bytesWritten);
        _position += bytesWritten;
        return bytesWritten;
    }

    bool seek(std::uint64_t offset) override {
        if (offset > 0xFFFFFFFFULL) {
            return false;
        }
        _position = offset;
        return true;
    }

    std::uint64_t tell() const override {
        return _position;
    }

    bool flush() override {
        // TNFS doesn't have a flush command, so this is a no-op
        return true;
    }

    std::size_t lane_count() const { return _lanes.size(); }

    void drop_lanes() {
        for (int h : _lanes) {
            _client->close(h);
        }
        if (!_lanes.empty()) {
            FN_LOGI(TAG, "%s: read-ahead handles given back", _path.c_str());
        }
        _lanes.clear();
        _lanesTried = true;
    }

private:
    struct CacheEntry {
        std::uint64_t offset{kUnknown};
        std::uint32_t lastUse{0};
    };

    bool sync_position() {
        if (_serverPos == _position) {
            return true;
        }
        if (!_client->seek(_fileHandle, static_cast<std::uint32_t>(_position))) {
            _serverPos = kUnknown;
            return false;
        }
        _serverPos = _position;
        return true;
    }

    CacheEntry* cache_find(std::uint64_t offset) {
        for (auto& e : _entries) {
            if (e.offset == offset) {
                return &e;
            }
        }
        return nullptr;
    }

    bool cache_get(std::uint64_t offset, std::uint8_t* out) {
        CacheEntry* e = cache_find(offset);
        if (!e) {
            return false;
        }
        e->lastUse = ++_useClock;
        std::memcpy(out, block_data(*e), kBlock);
        return true;
    }

    void cache_put(std::uint64_t offset, const std::uint8_t* in) {
        if (_entries.empty()) {
            _entries.resize(kCacheBlocks);
            _data.resize(kCacheBlocks * kBlock);
        }
        CacheEntry* e = cache_find(offset);
        if (!e) {
            e = &_entries[0];
            for (auto& c : _entries) {
                if (c.offset == kUnknown) {
                    e = &c;
                    break;
                }
                if (c.lastUse < e->lastUse) {
                    e = &c;
                }
            }
            e->offset = offset;
        }
        e->lastUse = ++_useClock;
        std::memcpy(block_data(*e), in, kBlock);
    }

    void cache_written(std::uint64_t offset, const std::uint8_t* in, std::size_t bytes) {
        if (_entries.empty() || bytes == 0) {
            return;
        }
        if (bytes == kBlock && offset % kBlock == 0) {
            if (cache_find(offset)) {
                cache_put(offset, in);
            }
            return;
        }
        for (auto& e : _entries) {
            if (e.offset != kUnknown && e.offset < offset + bytes && offset < e.offset + kBlock) {
                e.offset = kUnknown;
            }
        }
    }

    std::uint8_t* block_data(const CacheEntry& e) {
        return _data.data() + static_cast<std::size_t>(&e - _entries.data()) * kBlock;
    }

    bool read_ahead(std::uint64_t offset) {
        if (!_client->supports_parallel_reads()) {
            return false;
        }
        open_lanes();
        if (_lanes.empty()) {
            return false;
        }

        std::vector<std::uint8_t> buf((_lanes.size() + 1) * kBlock);
        std::vector<tnfs::ITnfsClient::ReadAt> reads;
        const std::size_t handles = _lanes.size() + 1;
        for (std::size_t i = 0; i < kReadAhead && reads.size() < handles; ++i) {
            const std::uint64_t at = offset + i * kBlock;
            if (i > 0 && cache_find(at)) {
                continue;
            }
            if (at + kBlock > 0xFFFFFFFFULL) {
                break;
            }
            tnfs::ITnfsClient::ReadAt r;
            r.fileHandle = reads.empty() ? _fileHandle : _lanes[reads.size() - 1];
            r.offset = static_cast<std::uint32_t>(at);
            r.dst = buf.data() + reads.size() * kBlock;
            r.bytes = kBlock;
            reads.push_back(r);
        }
        _client->read_parallel(reads);
        _serverPos = kUnknown; // the main handle took part

        bool gotFirst = false;
        for (const auto& r : reads) {
            if (r.got == kBlock) {
                cache_put(r.offset, r.dst);
                gotFirst = gotFirst || r.offset == offset;
            }
        }
        return gotFirst;
    }

    void open_lanes() {
        if (_lanesTried) {
            return;
        }
        _lanesTried = true;
        // The server limits a session's open files, so take what it gives.
        for (std::size_t i = 1; i < kReadAhead; ++i) {
            const int h = _client->open(_path, tnfs::OPENMODE_READ, 0);
            if (h < 0) {
                break;
            }
            _lanes.push_back(h);
        }
        FN_LOGI(TAG, "%s: reading ahead %u blocks at a time", _path.c_str(),
                static_cast<unsigned>(_lanes.size() + 1));
    }

    std::shared_ptr<tnfs::ITnfsClient> _client;
    int _fileHandle;
    std::string _path;
    bool _cacheable;
    std::uint64_t _position{0};
    std::uint64_t _serverPos{0};
    std::vector<int> _lanes;
    bool _lanesTried{false};
    std::vector<CacheEntry> _entries;
    std::vector<std::uint8_t> _data;
    std::uint32_t _useClock{0};
    std::shared_ptr<TnfsOpenFiles> _openFiles;
};

class TnfsFileSystem final : public IFileSystem {
public:
    explicit TnfsFileSystem(TnfsClientFactory clientFactory)
        : _clientFactory(std::move(clientFactory))
    {
        FN_LOGI(TAG, "TNFS filesystem created (dynamic endpoints)");
    }

    explicit TnfsFileSystem(std::shared_ptr<tnfs::ITnfsClient> fixedClient)
        : _fixedClient(std::move(fixedClient))
    {
        FN_LOGI(TAG, "TNFS filesystem created (single client)");
    }

    ~TnfsFileSystem() override = default;

    FileSystemKind kind() const override {
        return FileSystemKind::NetworkTnfs;
    }

    std::string name() const override {
        return "tnfs";
    }

    bool exists(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->exists(resolved.path);
    }

    bool isDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->isDirectory(resolved.path);
    }

    bool createDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->createDirectory(resolved.path);
    }

    bool removeFile(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->removeFile(resolved.path);
    }

    bool removeDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->removeDirectory(resolved.path);
    }

    bool rename(const std::string& from, const std::string& to) override {
        ResolvedPath src{};
        ResolvedPath dst{};
        if (!resolve_path(from, src) || !resolve_path(to, dst)) {
            return false;
        }

        if (src.endpoint.host != dst.endpoint.host ||
            src.endpoint.port != dst.endpoint.port ||
            src.endpoint.mountPath != dst.endpoint.mountPath ||
            src.endpoint.user != dst.endpoint.user ||
            src.endpoint.password != dst.endpoint.password) {
            FN_LOGE(TAG, "Rename across TNFS endpoints is not supported");
            return false;
        }

        return src.client->rename(src.path, dst.path);
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return nullptr;
        }
        uint16_t openMode = 0;
        // Default perms for newly created files (rw-r--r--).
        uint16_t createPerms = 0644;

        if (std::strchr(mode, 'r') != nullptr) {
            openMode |= tnfs::OPENMODE_READ;
        }
        if (std::strchr(mode, 'w') != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_TRUNCATE;
        }
        if (std::strchr(mode, 'a') != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_APPEND;
        }
        if (std::strchr(mode, '+') != nullptr) {
            openMode |= tnfs::OPENMODE_READWRITE;
        }
        if (openMode == 0) {
            // Default to read mode for unrecognized mode strings.
            openMode = tnfs::OPENMODE_READ;
        }

        auto& openFiles = _openFiles[resolved.client.get()];
        if (!openFiles) {
            openFiles = std::make_shared<TnfsOpenFiles>();
        }
        int fileHandle = resolved.client->open(resolved.path, openMode, createPerms);
        while (fileHandle == -1) {
            // The session may be out of handles.
            TnfsFile* most = nullptr;
            for (TnfsFile* f : openFiles->files) {
                if (f->lane_count() > 0 && (!most || f->lane_count() > most->lane_count())) {
                    most = f;
                }
            }
            if (!most) {
                break;
            }
            most->drop_lanes();
            fileHandle = resolved.client->open(resolved.path, openMode, createPerms);
        }
        if (fileHandle == -1) {
            FN_LOGE(TAG, "Failed to open file: %s", resolved.path.c_str());
            return nullptr;
        }

        // Appends go wherever the file ends, so they skip the block cache.
        const bool cacheable = std::strchr(mode, 'a') == nullptr;
        return std::make_unique<TnfsFile>(resolved.client, fileHandle, resolved.path, cacheable, openFiles);
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }

        tnfs::TnfsStat st{};
        if (!resolved.client->stat(resolved.path, st)) {
            return false;
        }

        outInfo.path = path;
        outInfo.isDirectory = st.isDir;
        outInfo.sizeBytes = st.filesize;
        outInfo.modifiedTime = std::chrono::system_clock::from_time_t(st.mTime);

        return true;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }

        if (!resolved.client->isDirectory(resolved.path)) {
            return false;
        }

        outEntries.clear();
        std::vector<std::string> entries = resolved.client->listDirectory(resolved.path);
        for (const auto& entryName : entries) {
            std::string entryPath = join_path(resolved.path, entryName);
            tnfs::TnfsStat st{};
            if (!resolved.client->stat(entryPath, st)) {
                continue;
            }

            FileInfo info{};
            info.path = entryPath;
            info.isDirectory = st.isDir;
            info.sizeBytes = st.filesize;
            info.modifiedTime = std::chrono::system_clock::from_time_t(st.mTime);
            outEntries.push_back(std::move(info));
        }

        return true;
    }

private:
    struct SessionKey {
        std::string host;
        std::uint16_t port{tnfs::DEFAULT_PORT};
        std::string mountPath;
        std::string user;
        std::string password;
        bool useTcp{false};

        bool operator<(const SessionKey& other) const
        {
            return std::tie(host, port, mountPath, user, password, useTcp) <
                   std::tie(other.host, other.port, other.mountPath, other.user, other.password, other.useTcp);
        }
    };

    struct Session {
        TnfsEndpoint endpoint;
        std::shared_ptr<tnfs::ITnfsClient> client;
    };

    struct ResolvedPath {
        TnfsEndpoint endpoint;
        std::string path;
        std::shared_ptr<tnfs::ITnfsClient> client;
    };

    static std::string ensure_abs_path(const std::string& path)
    {
        if (path.empty()) {
            return "/";
        }
        if (path.front() == '/') {
            return path;
        }
        return "/" + path;
    }

    static std::string join_path(const std::string& base, const std::string& name)
    {
        if (base.empty() || base == "/") {
            return "/" + name;
        }
        if (base.back() == '/') {
            return base + name;
        }
        return base + "/" + name;
    }

    static bool parse_host_port(const std::string& authority, std::string& outHost, std::uint16_t& outPort)
    {
        if (authority.empty()) {
            return false;
        }

        outHost = authority;
        outPort = tnfs::DEFAULT_PORT;

        // Handle [ipv6]:port syntax.
        if (authority.front() == '[') {
            std::size_t bracket = authority.find(']');
            if (bracket == std::string::npos) {
                return false;
            }
            outHost = authority.substr(1, bracket - 1);
            if (bracket + 1 < authority.size()) {
                if (authority[bracket + 1] != ':') {
                    return false;
                }
                std::string portStr = authority.substr(bracket + 2);
                if (portStr.empty()) {
                    return false;
                }
                unsigned long parsed = 0;
                try {
                    parsed = std::stoul(portStr);
                } catch (...) {
                    return false;
                }
                if (parsed == 0 || parsed > 65535UL) {
                    return false;
                }
                outPort = static_cast<std::uint16_t>(parsed);
            }
            return true;
        }

        std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            return true;
        }

        // If there are multiple colons it's likely bare IPv6 without brackets, keep as host.
        if (authority.find(':') != colon) {
            return true;
        }

        std::string host = authority.substr(0, colon);
        std::string portStr = authority.substr(colon + 1);
        if (host.empty() || portStr.empty()) {
            return false;
        }

        unsigned long parsed = 0;
        try {
            parsed = std::stoul(portStr);
        } catch (...) {
            return false;
        }
        if (parsed == 0 || parsed > 65535UL) {
            return false;
        }

        outHost = host;
        outPort = static_cast<std::uint16_t>(parsed);
        return true;
    }

    bool parse_endpoint_and_path(const std::string& rawPath, TnfsEndpoint& endpoint, std::string& outPath) const
    {
        endpoint = TnfsEndpoint{};

        // Preferred format: tnfs://host[:port]/path
        // Make case-insensitive
        std::string lower_path;
        lower_path.reserve(rawPath.size());
        for (char c : rawPath) {
            lower_path.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        
        if (lower_path.rfind("tnfs://", 0) == 0) {
            UriParts parts = parse_uri(rawPath);
            if (parts.scheme != "tnfs") {
                return false;
            }

            if (!parse_host_port(parts.authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in URI: %s", rawPath.c_str());
                return false;
            }

            outPath = ensure_abs_path(parts.path);
            return true;
        }

        // TCP TNFS form: tnfs+tcp://host[:port]/path (aliases: tnfstcp://, tnfs-tcp://)
        if (lower_path.rfind("tnfs+tcp://", 0) == 0 ||
            lower_path.rfind("tnfstcp://", 0) == 0 ||
            lower_path.rfind("tnfs-tcp://", 0) == 0) {
            UriParts parts = parse_uri(rawPath);
            if (parts.scheme != "tnfs+tcp" &&
                parts.scheme != "tnfstcp" &&
                parts.scheme != "tnfs-tcp") {
                return false;
            }

            if (!parse_host_port(parts.authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in URI: %s", rawPath.c_str());
                return false;
            }

            endpoint.useTcp = true;
            outPath = ensure_abs_path(parts.path);
            return true;
        }

        // Backward-compatible form: //host[:port]/path
        if (rawPath.rfind("//", 0) == 0) {
            std::size_t slash = rawPath.find('/', 2);
            std::string authority = (slash == std::string::npos)
                ? rawPath.substr(2)
                : rawPath.substr(2, slash - 2);

            if (!parse_host_port(authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in path: %s", rawPath.c_str());
                return false;
            }

            outPath = (slash == std::string::npos) ? "/" : rawPath.substr(slash);
            outPath = ensure_abs_path(outPath);
            return true;
        }

        // No endpoint info in path; use default route if configured.
        if (_fixedClient) {
            endpoint.host = "__fixed__";
            endpoint.port = tnfs::DEFAULT_PORT;
            endpoint.mountPath = "/";
            outPath = ensure_abs_path(rawPath);
            return true;
        }

        if (_defaultEndpoint.host.empty()) {
            FN_LOGE(TAG, "TNFS path is missing endpoint: %s", rawPath.c_str());
            return false;
        }

        endpoint = _defaultEndpoint;
        outPath = ensure_abs_path(rawPath);
        return true;
    }

    std::shared_ptr<tnfs::ITnfsClient> get_or_create_session(const TnfsEndpoint& endpoint)
    {
        SessionKey key{endpoint.host, endpoint.port, endpoint.mountPath, endpoint.user, endpoint.password, endpoint.useTcp};
        auto existing = _sessions.find(key);
        if (existing != _sessions.end()) {
            return existing->second.client;
        }

        if (_fixedClient) {
            if (_sessions.empty()) {
                if (!_fixedClient->mount(endpoint.mountPath, endpoint.user, endpoint.password)) {
                    FN_LOGE(TAG, "Failed to mount fixed TNFS client");
                    return {};
                }
                Session session{};
                session.endpoint = endpoint;
                session.client = _fixedClient;
                _sessions.emplace(std::move(key), std::move(session));
                return _fixedClient;
            }
            FN_LOGE(TAG, "Fixed TNFS client cannot switch endpoints");
            return {};
        }

        if (!_clientFactory) {
            FN_LOGE(TAG, "No TNFS client factory configured");
            return {};
        }

        std::unique_ptr<tnfs::ITnfsClient> created = _clientFactory(endpoint);
        if (!created) {
            FN_LOGE(TAG, "Failed to create TNFS client for %s:%u",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port));
            return {};
        }

        std::shared_ptr<tnfs::ITnfsClient> client(std::move(created));
        if (!client->mount(endpoint.mountPath, endpoint.user, endpoint.password)) {
            FN_LOGE(TAG, "Failed to mount TNFS session for %s:%u",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port));
            return {};
        }

        Session session{};
        session.endpoint = endpoint;
        session.client = client;
        _sessions.emplace(std::move(key), std::move(session));
        FN_LOGI(TAG, "Mounted TNFS session %s:%u",
            endpoint.host.c_str(),
            static_cast<unsigned>(endpoint.port));
        return client;
    }

    bool resolve_path(const std::string& rawPath, ResolvedPath& out)
    {
        if (!parse_endpoint_and_path(rawPath, out.endpoint, out.path)) {
            return false;
        }
        out.client = get_or_create_session(out.endpoint);
        return static_cast<bool>(out.client);
    }

    TnfsClientFactory _clientFactory;
    std::shared_ptr<tnfs::ITnfsClient> _fixedClient;
    TnfsEndpoint _defaultEndpoint;
    std::map<SessionKey, Session> _sessions;
    std::map<const tnfs::ITnfsClient*, std::shared_ptr<TnfsOpenFiles>> _openFiles;
};

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient> client) {
    if (!client) {
        return nullptr;
    }
    return std::make_unique<TnfsFileSystem>(std::move(client));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::unique_ptr<tnfs::ITnfsClient> client) {
    return make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient>(std::move(client)));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(TnfsClientFactory clientFactory)
{
    if (!clientFactory) {
        return nullptr;
    }
    return std::make_unique<TnfsFileSystem>(std::move(clientFactory));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem() {
    FN_LOGE(TAG, "make_tnfs_filesystem() without factory is not supported");
    return nullptr;
}

} // namespace fujinet::fs
