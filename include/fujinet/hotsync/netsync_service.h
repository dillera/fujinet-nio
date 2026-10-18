#pragma once

// Network HotSync (NetSync) server: listens on TCP 14238 and runs one sync per
// connection. A sync blocks on the Palm for minutes, so the service runs on a
// task of its own (platform::start_service_task), never in core.tick().

#include "fujinet/hotsync/netsync_transport.h"
#include "fujinet/hotsync/session.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace fujinet::fs {
class IFileSystem;
}

namespace fujinet::net {
class ITcpSocketOps;
}

namespace fujinet::hotsync {

struct NetSyncSettings {
    std::uint16_t port{kNetSyncPort};
    std::string root{"/hotsync"}; // folder on the filesystem given to the service
    std::string userName{"FujiNet"};
    BackupPolicy backup{BackupPolicy::Flagged};
    std::size_t maxDatabaseBytes{kDefaultMaxDatabaseBytes};
};

// What the diagnostics console shows. A copy, safe to read from any thread.
struct NetSyncStatus {
    bool listening{false};
    bool syncing{false};
    std::uint16_t port{0};
    std::uint32_t syncs{0};    // syncs started
    std::uint32_t failures{0}; // syncs that did not reach EndOfSync
    std::string lastTime;      // local time the last sync ended, or empty
    std::string lastUser;
    std::string lastResult;    // e.g. "ok" or "link failure"
    int lastInstalled{0};
    int lastInstallFailures{0};
    int lastBackedUp{0};
    int lastBackupFailures{0};
};

class NetSyncService {
public:
    // Local wall-clock time for the Palm's "last synced" stamp; year 0 when
    // the clock is not set.
    using Clock = std::function<DlpDateTime()>;

    NetSyncService(net::ITcpSocketOps& ops, fs::IFileSystem& fs, NetSyncSettings settings, Clock clock);
    ~NetSyncService();

    NetSyncService(const NetSyncService&) = delete;
    NetSyncService& operator=(const NetSyncService&) = delete;

    // One pass: listen if not yet listening, then serve one waiting Palm.
    // Returns true when it did something, false when idle.
    bool poll();
    // Polls until stop() is called. For the service task.
    void run();
    void stop() { _stop.store(true); }

    NetSyncStatus status() const;
    const NetSyncSettings& settings() const { return _settings; }

private:
    bool start_listening();
    void serve(int fd);
    std::string now_text() const;

    net::ITcpSocketOps& _ops;
    fs::IFileSystem& _fs;
    NetSyncSettings _settings;
    Clock _clock;
    int _listenFd{-1};
    std::uint64_t _nextListenAttemptMs{0};
    std::atomic_bool _stop{false};

    mutable std::mutex _statusMutex;
    NetSyncStatus _status;
};

} // namespace fujinet::hotsync
