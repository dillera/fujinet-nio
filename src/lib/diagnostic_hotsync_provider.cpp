#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/hotsync/hotsync_init.h"
#include "fujinet/hotsync/netsync_service.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::diag {

namespace {

void add_line(DiagResult& r, const char* key, const std::string& value)
{
    r.text += key;
    r.text += ": ";
    r.text += value;
    r.text += "\r\n";
    r.kv.emplace_back(key, value);
}

class HotSyncDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit HotSyncDiagnosticProvider(std::shared_ptr<hotsync::NetSyncService> service)
        : _service(std::move(service))
    {}

    std::string_view provider_id() const noexcept override { return "hotsync"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "hotsync.status",
            .summary = "show the HotSync service and the last sync",
            .usage = "hotsync.status",
            .safe = true,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        if (args.argv[0] == "hotsync.status") {
            return cmd_status();
        }
        return DiagResult::not_found("unknown hotsync command");
    }

private:
    DiagResult cmd_status() const
    {
        DiagResult r = DiagResult::ok();
        if (!_service) {
            add_line(r, "enabled", "0");
            r.text += "(set hotsync.enabled in fujinet.yaml and restart)\r\n";
            return r;
        }
        const hotsync::NetSyncSettings& settings = _service->settings();
        const hotsync::NetSyncStatus s = _service->status();
        add_line(r, "enabled", "1");
        add_line(r, "listening", s.listening ? "1" : "0");
        add_line(r, "port", std::to_string(s.port));
        add_line(r, "root", settings.root);
        add_line(r, "user_name", settings.userName);
        add_line(r, "backup", hotsync::backup_policy_name(settings.backup));
        add_line(r, "syncing", s.syncing ? "1" : "0");
        add_line(r, "syncs", std::to_string(s.syncs));
        add_line(r, "failures", std::to_string(s.failures));
        if (s.syncs > 0 && !s.syncing) {
            add_line(r, "last_time", s.lastTime.empty() ? "(clock not set)" : s.lastTime);
            add_line(r, "last_user", s.lastUser);
            add_line(r, "last_result", s.lastResult);
            add_line(r, "last_installed", std::to_string(s.lastInstalled));
            add_line(r, "last_install_failures", std::to_string(s.lastInstallFailures));
            add_line(r, "last_backed_up", std::to_string(s.lastBackedUp));
            add_line(r, "last_backup_failures", std::to_string(s.lastBackupFailures));
        }
        return r;
    }

    std::shared_ptr<hotsync::NetSyncService> _service;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_hotsync_diagnostic_provider(
    std::shared_ptr<hotsync::NetSyncService> service)
{
    return std::make_unique<HotSyncDiagnosticProvider>(std::move(service));
}

} // namespace fujinet::diag
