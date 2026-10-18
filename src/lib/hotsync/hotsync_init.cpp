#include "fujinet/hotsync/hotsync_init.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/logging.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/hotsync/netsync_service.h"
#include "fujinet/platform/service_task.h"
#include "fujinet/platform/tcp_socket_ops.h"
#include "fujinet/platform/time.h"

#include <ctime>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";
constexpr std::size_t kTaskStackBytes = 8192;

// Local time from the system clock and zone (clock.timezone).
DlpDateTime local_now()
{
    DlpDateTime t;
    if (!platform::time_is_valid()) {
        return t;
    }
    const auto now = static_cast<std::time_t>(platform::unix_time_seconds());
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        return t;
    }
    t.year = static_cast<std::uint16_t>(local.tm_year + 1900);
    t.month = static_cast<std::uint8_t>(local.tm_mon + 1);
    t.day = static_cast<std::uint8_t>(local.tm_mday);
    t.hour = static_cast<std::uint8_t>(local.tm_hour);
    t.minute = static_cast<std::uint8_t>(local.tm_min);
    t.second = static_cast<std::uint8_t>(local.tm_sec);
    return t;
}

void run_service(void* ctx)
{
    auto* service = static_cast<std::shared_ptr<NetSyncService>*>(ctx);
    (*service)->run();
    delete service;
}

} // namespace

bool parse_backup_policy(const std::string& text, BackupPolicy& out)
{
    if (text == "none") {
        out = BackupPolicy::None;
    } else if (text == "flagged") {
        out = BackupPolicy::Flagged;
    } else if (text == "all") {
        out = BackupPolicy::All;
    } else {
        return false;
    }
    return true;
}

const char* backup_policy_name(BackupPolicy policy)
{
    switch (policy) {
    case BackupPolicy::None: return "none";
    case BackupPolicy::Flagged: return "flagged";
    case BackupPolicy::All: return "all";
    }
    return "flagged";
}

std::shared_ptr<NetSyncService> start_netsync_service(fs::StorageManager& storage,
                                                      const config::HotSyncConfig& config)
{
    if (!config.enabled) {
        return nullptr;
    }
    fs::IFileSystem* fs = storage.defaultPersistentFileSystem();
    if (fs == nullptr) {
        FN_LOGW(TAG, "no persistent filesystem; HotSync is off");
        return nullptr;
    }

    NetSyncSettings settings;
    settings.port = config.port;
    settings.root = config.root.empty() ? "/hotsync" : config.root;
    settings.userName = config.userName.empty() ? "FujiNet" : config.userName;
    if (!parse_backup_policy(config.backup, settings.backup)) {
        FN_LOGW(TAG, "unknown hotsync.backup '%s'; using flagged", config.backup.c_str());
    }

    auto service = std::make_shared<NetSyncService>(platform::default_tcp_socket_ops(), *fs, settings,
                                                    &local_now);
    auto* taskRef = new std::shared_ptr<NetSyncService>(service);
    if (!platform::start_service_task("fn_hotsync", kTaskStackBytes, &run_service, taskRef)) {
        FN_LOGE(TAG, "cannot start the HotSync task");
        delete taskRef;
        return nullptr;
    }
    FN_LOGI(TAG, "HotSync on %s:%s, backup %s", fs->name().c_str(), settings.root.c_str(),
            backup_policy_name(settings.backup));
    return service;
}

} // namespace fujinet::hotsync
