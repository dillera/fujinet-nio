#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/wifi_controller.h"
#include "fujinet/io/devices/wifi_service.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_device_diagnostics.h"
#include "fujinet/io/devices/network_translation.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/net/network_link.h"
#include "fujinet/platform/image_translation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace {

static bool parse_u16(std::string_view s, std::uint16_t& out)
{
    // Accept decimal or 0x-prefixed hex.
    if (s.empty()) return false;

    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
        if (s.empty()) return false;
    }

    unsigned value = 0;
    for (char c : s) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else return false;

        value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
        if (value > 0xFFFFu) return false;
    }

    out = static_cast<std::uint16_t>(value);
    return true;
}

static std::string hex4(std::uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(v));
    return std::string(buf);
}

static const char* link_state_name(fujinet::net::LinkState st)
{
    using fujinet::net::LinkState;
    switch (st) {
    case LinkState::Disconnected: return "disconnected";
    case LinkState::Connecting:   return "connecting";
    case LinkState::Connected:    return "connected";
    case LinkState::Failed:     return "failed";
    }
    return "unknown";
}

static const char* translation_name(std::uint8_t raw)
{
    using fujinet::io::ContentTranslationType;
    switch (static_cast<ContentTranslationType>(raw)) {
        case ContentTranslationType::None:
            return "none";
        case ContentTranslationType::Json:
            return "json";
        case ContentTranslationType::Xml:
            return "xml";
        case ContentTranslationType::Rss:
            return "rss";
        case ContentTranslationType::Image:
            return "image";
    }
    return "unknown";
}

static const char* status_name(fujinet::io::StatusCode st)
{
    using fujinet::io::StatusCode;
    switch (st) {
        case StatusCode::Ok: return "ok";
        case StatusCode::DeviceNotFound: return "device_not_found";
        case StatusCode::InvalidRequest: return "invalid_request";
        case StatusCode::DeviceBusy: return "device_busy";
        case StatusCode::NotReady: return "not_ready";
        case StatusCode::IOError: return "io_error";
        case StatusCode::Timeout: return "timeout";
        case StatusCode::InternalError: return "internal_error";
        case StatusCode::Unsupported: return "unsupported";
    }
    return "unknown";
}

static fujinet::io::NetworkDevice* get_net_device(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;

    auto* dev = core.deviceManager().getDevice(to_device_id(WireDeviceId::NetworkService));
    return dynamic_cast<fujinet::io::NetworkDevice*>(dev);
}

static fujinet::io::WifiService* get_wifi_service(fujinet::core::FujinetCore& core)
{
    using fujinet::io::protocol::WireDeviceId;
    using fujinet::io::protocol::to_device_id;
    return dynamic_cast<fujinet::io::WifiService*>(
        core.deviceManager().getDevice(to_device_id(WireDeviceId::WifiService)));
}

static const char* join_state_name(std::uint8_t state)
{
    switch (state) {
    case 0: return "idle";
    case 1: return "connecting";
    case 2: return "succeeded";
    case 3: return "failed; previous network restore attempted";
    default: return "unknown";
    }
}

class NetworkDiagnosticProvider final : public IDiagnosticProvider {
public:
    NetworkDiagnosticProvider(fujinet::core::FujinetCore& core, std::shared_ptr<NetworkDiagWifiContext> wifi_ctx)
        : _core(core)
        , _wifi_ctx(std::move(wifi_ctx))
    {}

    std::string_view provider_id() const noexcept override { return "net"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "net.sessions",
            .summary = "list active network sessions/handles",
            .usage = "net.sessions",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.close",
            .summary = "close a session handle (or all)",
            .usage = "net.close <handle|all>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.translation.get",
            .summary = "show content translation settings (Image pixel cap: live, stored and platform default)",
            .usage = "net.translation.get",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.translation.set",
            .summary = "set a content translation setting for new translations and store it",
            .usage = "net.translation.set image_max_pixels <pixels|default>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.translation.stats",
            .summary = "recent content translations: time, sizes, stack and heap used",
            .usage = "net.translation.stats",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "net.translation.save",
            .summary = "write content translation settings into fujinet.yaml (requires FujiDevice)",
            .usage = "net.translation.save",
            .safe = false,
        });
        if (_wifi_ctx) {
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.scan",
                .summary = "scan for nearby Wi-Fi access points",
                .usage = "net.wifi.scan",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.status",
                .summary = "show configured and live Wi-Fi state",
                .usage = "net.wifi.status",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.get",
                .summary = "show configured Wi-Fi SSID and password presence",
                .usage = "net.wifi.get",
                .safe = true,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.set",
                .summary = "set configured SSID or password, save, and reconnect",
                .usage = "net.wifi.set <ssid|bssid|password|passphrase> <value...>",
                .safe = false,
            });
            out.push_back(DiagCommandSpec{
                .name = "net.wifi.save",
                .summary = "persist current Wi-Fi settings to fujinet.yaml",
                .usage = "net.wifi.save",
                .safe = false,
            });
        }
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "net.sessions") {
            return cmd_sessions();
        }
        if (cmd == "net.close") {
            return cmd_close(args);
        }
        if (cmd == "net.translation.get") {
            return cmd_translation_get();
        }
        if (cmd == "net.translation.set") {
            return cmd_translation_set(args);
        }
        if (cmd == "net.translation.save") {
            return cmd_translation_save();
        }
        if (cmd == "net.translation.stats") {
            return cmd_translation_stats();
        }
        if (_wifi_ctx) {
            if (cmd == "net.wifi.scan") {
                return cmd_wifi_scan();
            }
            if (cmd == "net.wifi.status") {
                return cmd_wifi_status();
            }
            if (cmd == "net.wifi.get") {
                return cmd_wifi_get();
            }
            if (cmd == "net.wifi.set") {
                return cmd_wifi_set(args);
            }
            if (cmd == "net.wifi.save") {
                return cmd_wifi_save();
            }
        }

        return DiagResult::not_found("unknown net command");
    }

private:
    fujinet::io::FujiDevice* fuji_device() const
    {
        return _wifi_ctx ? _wifi_ctx->fuji : nullptr;
    }

    fujinet::io::WifiController wifi_controller() const
    {
        auto* fuji = fuji_device();
        return fujinet::io::WifiController(
            fuji->config_mut(), fuji->config_store(), _wifi_ctx->ensure_wifi);
    }

    static std::string join_args(const DiagArgsView& args, std::size_t from)
    {
        std::string out;
        for (std::size_t i = from; i < args.argv.size(); ++i) {
            if (i > from) {
                out.push_back(' ');
            }
            out.append(args.argv[i].data(), args.argv[i].size());
        }
        return out;
    }

    DiagResult cmd_wifi_get()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        auto controller = wifi_controller();
        const auto& wifi_cfg = controller.config();
        std::string text;
        text += "enabled: ";
        text += (wifi_cfg.enabled ? "1" : "0");
        text += "\r\nssid: ";
        text += wifi_cfg.ssid;
        text += "\r\nbssid: ";
        text += wifi_cfg.bssid;
        text += "\r\npassword_present: ";
        text += wifi_cfg.passphrase.empty() ? "0\r\n" : "1\r\n";

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("enabled", wifi_cfg.enabled ? "1" : "0");
        r.kv.emplace_back("ssid", wifi_cfg.ssid);
        r.kv.emplace_back("password_present", wifi_cfg.passphrase.empty() ? "0" : "1");
        return r;
    }

    DiagResult cmd_wifi_status()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        auto controller = wifi_controller();
        const auto& wifi_cfg = controller.config();
        auto* link = controller.link();

        std::string text;
        text += "configured_enabled: ";
        text += (wifi_cfg.enabled ? "1" : "0");
        text += "\r\nconfigured_ssid: ";
        text += wifi_cfg.ssid;
        text += "\r\njoin_trial: ";
        auto* service = get_wifi_service(_core);
        const char* trial = service ? join_state_name(service->join_status()) : "unavailable";
        text += trial;
        text += "\r\n";

        if (!link) {
            text += "link_state: unavailable\r\n";
            return DiagResult::ok(std::move(text));
        }

        const std::string mac = net::to_string(link->mac_address());
        text += "link_state: ";
        text += link_state_name(link->state());
        text += "\r\nmac: ";
        text += mac.empty() ? "unavailable" : mac;
        text += "\r\nip: ";
        text += link->ip_address();
        text += "\r\nrssi: ";
        text += std::to_string(link->rssi());
        text += "\r\nsubnet: ";
        text += link->subnet_mask();
        text += "\r\ngateway: ";
        text += link->gateway();
        text += "\r\ndns: ";
        text += link->dns_server();
        text += "\r\ncapabilities: ";
        text += std::to_string(link->capabilities().flags);
        text += "\r\n";

        DiagResult r = DiagResult::ok(text);
        r.kv.emplace_back("join_trial", trial);
        r.kv.emplace_back("link_state", link_state_name(link->state()));
        r.kv.emplace_back("ip", link->ip_address());
        r.kv.emplace_back("mac", mac);
        return r;
    }

    DiagResult cmd_wifi_set(const DiagArgsView& args)
    {
        auto* service = get_wifi_service(_core);
        if (service && service->join_status() == 1)
            return DiagResult::busy("Wi-Fi trial in progress");
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        if (args.argv.size() < 3) {
            return DiagResult::invalid_args("usage: net.wifi.set <ssid|bssid|password|passphrase> <value...>");
        }

        const std::string_view field = args.argv[1];
        const std::string value = join_args(args, 2);
        if (value.empty()) {
            return DiagResult::invalid_args("value must not be empty");
        }

        auto controller = wifi_controller();
        auto next = controller.config();
        bool changed = false;
        const bool is_password_field =
            ascii_iequals(field, "password") || ascii_iequals(field, "passphrase");

        if (ascii_iequals(field, "ssid")) {
            if (next.ssid != value) {
                next.ssid = value;
                changed = true;
            }
        } else if (ascii_iequals(field, "bssid")) {
            if (next.bssid != value) {
                next.bssid = value;
                changed = true;
            }
        } else if (is_password_field) {
            if (next.passphrase != value) {
                next.passphrase = value;
                changed = true;
            }
        } else {
            return DiagResult::invalid_args("field must be ssid, bssid, password, or passphrase");
        }

        if (!changed) {
            return DiagResult::ok("unchanged\r\n");
        }

        next.enabled = true;

        std::string text;
        if (is_password_field) {
            text += "password updated\r\n";
        } else if (ascii_iequals(field, "bssid")) {
            text += "bssid updated\r\n";
        } else {
            text += "ssid updated\r\n";
        }

        const auto updateStatus = controller.update(next, true, false);
        if (updateStatus == fujinet::io::StatusCode::InvalidRequest)
            return DiagResult::invalid_args("invalid Wi-Fi configuration");
        if (updateStatus == fujinet::io::StatusCode::Unsupported)
            return DiagResult::error("Wi-Fi configuration is unsupported\r\n");
        if (updateStatus != fujinet::io::StatusCode::Ok)
            return DiagResult::not_ready("Wi-Fi configuration could not be saved");

        const auto reconnectStatus = controller.reconnect();
        if (reconnectStatus == fujinet::io::StatusCode::Ok) {
            text += "reconnecting to ssid='";
            text += controller.config().ssid;
            text += "'\r\n";
        } else {
            text += "warning: Wi-Fi reconnect unavailable; settings saved\r\n";
        }

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("ssid", controller.config().ssid);
        return r;
    }

    DiagResult cmd_wifi_save()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }

        auto controller = wifi_controller();
        if (controller.save() != fujinet::io::StatusCode::Ok)
            return DiagResult::not_ready("config store not available");
        return DiagResult::ok("saved wifi settings to config store\r\n");
    }

    DiagResult cmd_wifi_scan()
    {
        auto* service = get_wifi_service(_core);
        if (service && service->join_status() == 1)
            return DiagResult::busy("Wi-Fi trial in progress");
        auto controller = wifi_controller();
        const auto scan = controller.scan();
        if (!scan.success) {
            return DiagResult::not_ready("Wi-Fi scan unavailable\r\n");
        }

        std::string text;
        text.reserve(scan.records.size() * 72 + 32);
        text += "ap_count: ";
        text += std::to_string(scan.records.size());
        text += "\r\n";

        for (const auto& ap : scan.records) {
            text += "ssid=";
            text += ap.ssid;
            text += " rssi=";
            text += std::to_string(ap.rssi);
            text += " channel=";
            text += std::to_string(ap.channel);
            text += " auth=";
            text += std::to_string(ap.auth);
            text += "\r\n";
        }

        if (scan.records.empty()) {
            text += "note: scan completed but no access points were reported\r\n";
        }

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("ap_count", std::to_string(scan.records.size()));
        return r;
    }

    DiagResult cmd_sessions()
    {
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        const auto rows = fujinet::io::NetworkDeviceDiagnosticsAccessor::sessions(*net);

        std::string text;
        text.reserve(256);

        std::size_t active = 0;
        for (const auto& r : rows) {
            if (!r.active) continue;
            ++active;
        }

        text += "active_sessions: ";
        text += std::to_string(active);
        text += "\r\n";

        for (const auto& r : rows) {
            if (!r.active) continue;

            text += "handle=";
            text += hex4(r.handle);
            text += " method=";
            text += std::to_string(r.method);
            text += " flags=";
            text += std::to_string(r.flags);
            text += " awaiting_body=";
            text += (r.awaitingBody ? "1" : "0");
            text += " body=";
            text += std::to_string(r.receivedBodyLen);
            text += "/";
            text += std::to_string(r.expectedBodyLen);
            text += " completed=";
            text += (r.completed ? "1" : "0");
            text += " idle_s=";
            text += std::to_string(r.idleMs / 1000);
            text += " keep=";
            text += (r.keepAlive ? "1" : "0");
            text += " url=";
            text += r.url;
            text += " translation=";
            text += translation_name(r.translationType);
            if (r.translationType != static_cast<std::uint8_t>(fujinet::io::ContentTranslationType::None)) {
                text += " selector=";
                text += r.translationSelector;
                text += " ready=";
                text += (r.translationReady ? "1" : "0");
                text += " translated=";
                text += std::to_string(r.translatedSize);
            }
            text += "\r\n";
        }

        DiagResult res = DiagResult::ok(text);
        res.kv.emplace_back("active_sessions", std::to_string(active));
        return res;
    }

    DiagResult cmd_close(const DiagArgsView& args)
    {
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        // args.argv[0] = "net.close", args.argv[1] = handle|all
        if (args.argv.size() < 2) {
            return DiagResult::invalid_args("usage: net.close <handle|all>");
        }

        const std::string_view target = args.argv[1];
        if (target == "all") {
            const std::size_t n = fujinet::io::NetworkDeviceDiagnosticsAccessor::close_all(*net);
            DiagResult r = DiagResult::ok("closed: " + std::to_string(n) + "\r\n");
            r.kv.emplace_back("closed", std::to_string(n));
            return r;
        }

        std::uint16_t handle = 0;
        if (!parse_u16(target, handle)) {
            return DiagResult::invalid_args("invalid handle (expected decimal or 0xHHHH)");
        }

        const bool ok = fujinet::io::NetworkDeviceDiagnosticsAccessor::close(*net, handle);
        if (!ok) {
            return DiagResult::error("close failed (invalid handle?)\r\n");
        }

        DiagResult r = DiagResult::ok("closed: " + hex4(handle) + "\r\n");
        r.kv.emplace_back("closed_handle", hex4(handle));
        return r;
    }

    // Upper bound for image_max_pixels: stb_image refuses sides above
    // STBI_MAX_DIMENSIONS (8192), so a larger cap could never be reached.
    static constexpr std::uint32_t kMaxImagePixels = 8192u * 8192u;

    DiagResult cmd_translation_get()
    {
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        const std::uint32_t live = fujinet::io::NetworkDeviceDiagnosticsAccessor::image_max_pixels(*net);
        std::string text;
        text += "image_max_pixels: ";
        text += std::to_string(live);
        text += "\r\n";
        if (auto* fuji = fuji_device()) {
            const std::uint32_t stored = fuji->config().translation.image.maxPixels;
            text += "stored_image_max_pixels: ";
            text += stored == 0 ? std::string("default") : std::to_string(stored);
            text += "\r\n";
        }
        text += "platform_default_image_max_pixels: ";
        text += std::to_string(fujinet::platform::default_image_max_pixels());
        text += "\r\n";

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("image_max_pixels", std::to_string(live));
        return r;
    }

    DiagResult cmd_translation_set(const DiagArgsView& args)
    {
        if (args.argv.size() < 3) {
            return DiagResult::invalid_args("usage: net.translation.set image_max_pixels <pixels|default>");
        }
        if (!ascii_iequals(args.argv[1], "image_max_pixels")) {
            return DiagResult::invalid_args("unknown field (image_max_pixels)");
        }

        // `default` stores 0, which means the platform's value.
        std::uint32_t stored = 0;
        if (!ascii_iequals(args.argv[2], "default")) {
            if (!parse_decimal_u32(args.argv[2], stored) || stored < 1 || stored > kMaxImagePixels) {
                return DiagResult::invalid_args("image_max_pixels must be 1.." + std::to_string(kMaxImagePixels) +
                                                " or default");
            }
        }
        const std::uint32_t live = stored != 0 ? stored : fujinet::platform::default_image_max_pixels();

        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }
        fujinet::io::NetworkDeviceDiagnosticsAccessor::set_image_max_pixels(*net, live);

        std::string text = "image_max_pixels set to " + std::to_string(live) + " for new translations";
        if (auto* fuji = fuji_device()) {
            fuji->config_mut().translation.image.maxPixels = stored;
            text += "; net.translation.save to keep it";
        }
        text += "\r\n";
        return DiagResult::ok(std::move(text));
    }

    // One line per logged translation, newest first. Stack and heap figures
    // come from the platform's large-stack runner; 0 means not measured.
    DiagResult cmd_translation_stats()
    {
        using fujinet::io::NetworkDeviceDiagnosticsAccessor;
        auto* net = get_net_device(_core);
        if (!net) {
            return DiagResult::not_ready("NetworkDevice not registered");
        }

        const auto totals = NetworkDeviceDiagnosticsAccessor::translation_totals(*net);
        std::string text;
        text += "translations: " + std::to_string(totals.count) + "\r\n";
        text += "failed: " + std::to_string(totals.failures) + "\r\n";

        std::size_t maxStack = 0;
        std::size_t maxHeapPeak = 0;
        for (const auto& e : NetworkDeviceDiagnosticsAccessor::translation_log(*net)) {
            const auto& st = e.stats.stack;
            const std::size_t heapPeak = st.heapLowestFreeBytes != 0 && st.heapFreeBeforeBytes > st.heapLowestFreeBytes
                ? st.heapFreeBeforeBytes - st.heapLowestFreeBytes
                : 0;
            maxStack = std::max(maxStack, st.stackUsedBytes);
            maxHeapPeak = std::max(maxHeapPeak, heapPeak);

            text += "type=";
            text += translation_name(static_cast<std::uint8_t>(e.type));
            text += " status=";
            text += status_name(e.status);
            text += " ms=" + std::to_string(e.elapsedMs);
            text += " body=" + std::to_string(e.bodyBytes);
            text += " out=" + std::to_string(e.translatedBytes);
            text += " stack_used=" + std::to_string(st.stackUsedBytes);
            text += " heap_peak=" + std::to_string(heapPeak);
            text += " heap_free_before=" + std::to_string(st.heapFreeBeforeBytes);
            text += " heap_largest_block=" + std::to_string(st.heapLargestBlockBytes);
            if (!e.stats.detail.empty()) {
                text += " : ";
                text += e.stats.detail;
            }
            text += "\r\n";
        }

        DiagResult r = DiagResult::ok(std::move(text));
        r.kv.emplace_back("translations", std::to_string(totals.count));
        r.kv.emplace_back("failed", std::to_string(totals.failures));
        r.kv.emplace_back("max_stack_used", std::to_string(maxStack));
        r.kv.emplace_back("max_heap_peak", std::to_string(maxHeapPeak));
        return r;
    }

    DiagResult cmd_translation_save()
    {
        auto* fuji = fuji_device();
        if (!fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }
        auto* store = fuji->config_store();
        if (!store) {
            return DiagResult::not_ready("config store not available");
        }
        store->save(fuji->config());
        return DiagResult::ok("saved translation settings to config store\r\n");
    }

    fujinet::core::FujinetCore& _core;
    std::shared_ptr<NetworkDiagWifiContext> _wifi_ctx;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_network_diagnostic_provider(
    ::fujinet::core::FujinetCore& core,
    std::shared_ptr<NetworkDiagWifiContext> wifi_ctx)
{
    return std::make_unique<NetworkDiagnosticProvider>(core, std::move(wifi_ctx));
}

} // namespace fujinet::diag
