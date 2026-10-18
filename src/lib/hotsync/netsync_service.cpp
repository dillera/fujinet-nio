#include "fujinet/hotsync/netsync_service.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/hotsync/fs_storage.h"
#include "fujinet/hotsync/netsync_transport.h"
#include "fujinet/hotsync/tcp_link.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

// Until the network is up a bind fails; try again this often.
constexpr std::uint64_t kListenRetryMs = 5000;
constexpr auto kIdlePoll = std::chrono::milliseconds(100);

std::uint32_t new_user_id()
{
    std::random_device rd;
    std::uint32_t id = 0;
    while (id == 0) {
        id = rd();
    }
    return id;
}

} // namespace

NetSyncService::NetSyncService(net::ITcpSocketOps& ops, fs::IFileSystem& fs, NetSyncSettings settings,
                               Clock clock)
    : _ops(ops), _fs(fs), _settings(std::move(settings)), _clock(std::move(clock))
{
    _status.port = _settings.port;
    FsStorage(_fs, _settings.root).create_install_folder();
}

NetSyncService::~NetSyncService()
{
    if (_listenFd >= 0) {
        _ops.close(_listenFd);
    }
}

bool NetSyncService::start_listening()
{
    const std::string port = std::to_string(_settings.port);
    net::AddrInfo* res = nullptr;
    if (_ops.getaddrinfo(nullptr, port.c_str(), _ops.tcp_stream_passive_addrinfo_hints(), &res) != 0 || !res) {
        if (res) {
            _ops.freeaddrinfo(res);
        }
        return false;
    }

    int fd = -1;
    for (auto* ai = res; ai; ai = _ops.addrinfo_next(ai)) {
        fd = _ops.socket(_ops.addrinfo_family(ai), _ops.addrinfo_socktype(ai), _ops.addrinfo_protocol(ai));
        if (fd < 0) {
            continue;
        }
        _ops.apply_listen_socket_options(fd);
        net::SockLen addrlen = 0;
        const struct sockaddr* addr = _ops.addrinfo_addr(ai, &addrlen);
        if (_ops.bind(fd, addr, addrlen) == 0 && _ops.listen(fd, 1) == 0) {
            (void)_ops.set_nonblocking(fd);
            break;
        }
        _ops.close(fd);
        fd = -1;
    }
    _ops.freeaddrinfo(res);

    if (fd < 0) {
        return false;
    }
    _listenFd = fd;
    FN_LOGI(TAG, "NetSync listening on TCP %u", static_cast<unsigned>(_settings.port));
    std::lock_guard<std::mutex> lock(_statusMutex);
    _status.listening = true;
    return true;
}

bool NetSyncService::poll()
{
    if (_listenFd < 0) {
        const std::uint64_t now = _ops.now_ms();
        if (now < _nextListenAttemptMs) {
            return false;
        }
        if (!start_listening()) {
            _nextListenAttemptMs = now + kListenRetryMs;
            return false;
        }
    }

    const int fd = _ops.accept(_listenFd, nullptr, nullptr);
    if (fd < 0) {
        return false;
    }
    (void)_ops.set_nonblocking(fd);
    _ops.apply_stream_socket_options(fd, /*nodelay=*/true, /*keepalive=*/true);
    serve(fd);
    return true;
}

void NetSyncService::run()
{
    while (!_stop.load()) {
        if (!poll()) {
            std::this_thread::sleep_for(kIdlePoll);
        }
    }
}

void NetSyncService::serve(int fd)
{
    {
        std::lock_guard<std::mutex> lock(_statusMutex);
        _status.syncing = true;
        ++_status.syncs;
    }
    FN_LOGI(TAG, "NetSync connection");

    TcpLink link(_ops, fd);
    NetSyncTransport transport(link);
    SessionReport report;
    if (transport.accept()) {
        FsStorage storage(_fs, _settings.root);
        SessionOptions options;
        options.defaultUserName = _settings.userName;
        options.newUserId = new_user_id();
        options.backup = _settings.backup;
        options.maxDatabaseBytes = _settings.maxDatabaseBytes;
        if (_clock) {
            options.now = _clock();
        }
        report = Session(transport, storage, options).run();
    } else {
        report.error = DlpError::Transport;
    }

    std::lock_guard<std::mutex> lock(_statusMutex);
    _status.syncing = false;
    if (report.error != DlpError::None) {
        ++_status.failures;
    }
    _status.lastTime = now_text();
    _status.lastUser = report.userName;
    _status.lastResult = report.error == DlpError::None ? "ok" : dlp_error_name(report.error);
    _status.lastInstalled = report.installed;
    _status.lastInstallFailures = report.installFailures;
    _status.lastBackedUp = report.backedUp;
    _status.lastBackupFailures = report.backupFailures;
}

std::string NetSyncService::now_text() const
{
    if (!_clock) {
        return {};
    }
    const DlpDateTime t = _clock();
    if (t.year == 0) {
        return {};
    }
    char text[40]; // room for any u8/u16 values, not just valid dates
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour,
                  t.minute, t.second);
    return text;
}

NetSyncStatus NetSyncService::status() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _status;
}

} // namespace fujinet::hotsync
