#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace fujinet::config {
struct HotSyncConfig;
}

namespace fujinet::fs {
class StorageManager;
}

namespace fujinet::hotsync {

class NetSyncService;
enum class BackupPolicy : std::uint8_t;

// Parses `hotsync.backup`; false for an unknown value.
bool parse_backup_policy(const std::string& text, BackupPolicy& out);
const char* backup_policy_name(BackupPolicy policy);

// Starts the NetSync service on a task of its own when config.enabled, with
// its folder on the persistent filesystem. Returns null when HotSync is off or
// cannot start (no persistent filesystem, no task). The task keeps its own
// reference, so the service outlives the returned pointer.
std::shared_ptr<NetSyncService> start_netsync_service(fs::StorageManager& storage,
                                                      const config::HotSyncConfig& config);

} // namespace fujinet::hotsync
