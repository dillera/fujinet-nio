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

namespace fujinet::io {
class Channel;
}

namespace fujinet::hotsync {

class CradleChannel;
class HotSyncService;
enum class BackupPolicy : std::uint8_t;

// Parses `hotsync.backup`; false for an unknown value.
bool parse_backup_policy(const std::string& text, BackupPolicy& out);
const char* backup_policy_name(BackupPolicy policy);

// With HotSync and its cradle on, wraps the FujiBus line in a CradleChannel
// and points `cradle` at it; the line must change rate (io::BaudControl).
// Otherwise returns `line` unchanged and leaves `cradle` null.
std::unique_ptr<io::Channel> attach_cradle(std::unique_ptr<io::Channel> line,
                                           const config::HotSyncConfig& config, CradleChannel*& cradle);

// Starts the HotSync service on a task of its own when config.enabled, with
// its folder on the persistent filesystem: NetSync unless config.port is 0,
// and the cradle when `cradle` is given (from attach_cradle). Returns null when
// HotSync is off or cannot start (no persistent filesystem, no task). The task
// keeps its own reference, so the service outlives the returned pointer.
std::shared_ptr<HotSyncService> start_hotsync_service(fs::StorageManager& storage,
                                                      const config::HotSyncConfig& config,
                                                      CradleChannel* cradle);

} // namespace fujinet::hotsync
