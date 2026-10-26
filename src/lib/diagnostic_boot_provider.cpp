#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/devices/fuji_device.h"

#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

const char* mode_name(config::BootMode mode)
{
    switch (mode) {
    case config::BootMode::Normal: return "normal";
    case config::BootMode::Config: return "config";
    case config::BootMode::Unknown: break;
    }
    return "unknown";
}

bool parse_bool(std::string_view text, bool& out)
{
    for (const char* yes : {"1", "true", "yes", "on"}) {
        if (ascii_iequals(text, yes)) {
            out = true;
            return true;
        }
    }
    for (const char* no : {"0", "false", "no", "off"}) {
        if (ascii_iequals(text, no)) {
            out = false;
            return true;
        }
    }
    return false;
}

class BootDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit BootDiagnosticProvider(fujinet::io::FujiDevice& fuji) : _fuji(fuji) {}

    std::string_view provider_id() const noexcept override { return "boot"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "boot.get",
            .summary = "show the boot configuration (boot.mode, boot.config_uri, boot.readonly)",
            .usage = "boot.get",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "boot.set",
            .summary = "change a boot setting; boot.save, then restart to apply",
            .usage = "boot.set <mode|config_uri|readonly> <value>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "boot.save",
            .summary = "write the boot settings into fujinet.yaml",
            .usage = "boot.save",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        const std::string_view cmd = args.argv[0];
        if (cmd == "boot.get") {
            return cmd_get();
        }
        if (cmd == "boot.set") {
            return cmd_set(args);
        }
        if (cmd == "boot.save") {
            return cmd_save();
        }
        return DiagResult::not_found("unknown boot command");
    }

private:
    DiagResult cmd_get()
    {
        const config::BootConfig& boot = _fuji.config().boot;
        std::string text;
        text += "mode: ";
        text += mode_name(boot.mode);
        text += "\r\nconfig_uri: ";
        text += boot.configUri;
        text += "\r\nreadonly: ";
        text += boot.readOnly ? "true" : "false";
        text += "\r\n";
        return DiagResult::ok(std::move(text));
    }

    DiagResult cmd_set(const DiagArgsView& args)
    {
        if (args.argv.size() < 3) {
            return DiagResult::invalid_args("usage: boot.set <mode|config_uri|readonly> <value>");
        }
        const std::string_view field = args.argv[1];
        const std::string_view value = args.argv[2];
        config::BootConfig& boot = _fuji.config_mut().boot;

        if (ascii_iequals(field, "mode")) {
            if (ascii_iequals(value, "normal")) {
                boot.mode = config::BootMode::Normal;
            } else if (ascii_iequals(value, "config")) {
                boot.mode = config::BootMode::Config;
            } else {
                return DiagResult::invalid_args("mode must be normal or config");
            }
        } else if (ascii_iequals(field, "config_uri")) {
            if (value.empty()) {
                return DiagResult::invalid_args("config_uri must not be empty");
            }
            boot.configUri = std::string(value);
        } else if (ascii_iequals(field, "readonly")) {
            bool flag = false;
            if (!parse_bool(value, flag)) {
                return DiagResult::invalid_args("readonly must be true or false");
            }
            boot.readOnly = flag;
        } else {
            return DiagResult::invalid_args("unknown field (mode, config_uri, readonly)");
        }
        return DiagResult::ok("boot." + std::string(field) + " set; boot.save, then restart to apply");
    }

    DiagResult cmd_save()
    {
        auto* store = _fuji.config_store();
        if (!store) {
            return DiagResult::not_ready("config store not available");
        }
        store->save(_fuji.config());
        return DiagResult::ok("saved boot settings to the config store; restart to apply");
    }

    fujinet::io::FujiDevice& _fuji;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_boot_diagnostic_provider(::fujinet::io::FujiDevice* fuji)
{
    if (fuji == nullptr) {
        return nullptr;
    }
    return std::make_unique<BootDiagnosticProvider>(*fuji);
}

} // namespace fujinet::diag
