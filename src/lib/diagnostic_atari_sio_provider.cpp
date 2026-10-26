// Only the physical Atari SIO build registers this provider; host builds keep it so the unit
// tests can run it. Every other build compiles this file to nothing.
#if defined(FN_BUILD_ATARI_FUJIBUS_SIO) || defined(FN_PLATFORM_POSIX)

#include "fujinet/diag/diagnostic_atari_sio.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fujinet::diag {

namespace {

std::string hex2(std::uint8_t v)
{
    static const char digits[] = "0123456789abcdef";
    std::string s = "0x";
    s += digits[v >> 4];
    s += digits[v & 0x0f];
    return s;
}

// Names the SIO commands an Atari disk or the native endpoint is likely to send.
const char* command_name(std::uint8_t device, std::uint8_t command)
{
    if (device == 0x7f) {
        switch (command) {
        case 'Q': return "query";
        case 'W': return "write";
        case 'P': return "poll";
        case 'R': return "read";
        default: return nullptr;
        }
    }
    switch (command) {
    case 'S': return "status";
    case 'R': return "read sector";
    case 'W': return "write sector";
    case 'P': return "put sector";
    case 'N': return "read PERCOM";
    case 'O': return "write PERCOM";
    case '!': return "format";
    case '"': return "format medium";
    default: return nullptr;
    }
}

void line(std::string& out, std::string_view text)
{
    out += text;
    out += "\r\n";
}

class AtariSioDiagnosticProvider final : public IDiagnosticProvider {
public:
    AtariSioDiagnosticProvider(std::function<io::AtariSioStats()> snapshot, std::function<void()> reset)
        : _snapshot(std::move(snapshot)), _reset(std::move(reset))
    {}

    std::string_view provider_id() const noexcept override { return "sio"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "sio.status",
            .summary = "Atari SIO ingress counters: CMD line, UART, frames, aborts, last command",
            .usage = "sio.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "sio.reset",
            .summary = "zero the SIO counters (do this before powering the Atari)",
            .usage = "sio.reset",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }
        const std::string_view cmd = args.argv[0];
        if (cmd == "sio.status") {
            return status();
        }
        if (cmd == "sio.reset") {
            _reset();
            return DiagResult::ok("sio counters reset\r\n");
        }
        return DiagResult::not_found("unknown sio command");
    }

private:
    DiagResult status() const
    {
        const io::AtariSioStats s = _snapshot();
        const auto n = [](std::uint32_t v) { return std::to_string(v); };
        std::string text;
        line(text, "cmd_edges: " + n(s.cmd_edges) + "   cmd_line: " + (s.cmd_line_low ? "low (asserted)" : "high (idle)"));
        line(text, "rx_bytes: " + n(s.capture.rx_bytes) + "   uart_rx_errors: " + n(s.uart_rx_errors));
        line(text, "frames: " + n(s.capture.frames) + "   disk(D1:-D8:): " + n(s.router.disk_commands) +
                       "   nio(0x7f): " + n(s.router.nio_commands) + "   other devices: " + n(s.router.other_commands));
        line(text, "aborts: edge=" + n(s.capture.aborts_edge) + " timeout=" + n(s.capture.aborts_timeout) +
                       " overflow=" + n(s.capture.aborts_overflow) + " rx_error=" + n(s.capture.aborts_rx_error));
        if (s.router.has_last_frame) {
            const auto& f = s.router.last_frame;
            std::string last = "last_frame: ";
            for (std::size_t i = 0; i < f.size(); ++i) {
                last += (i ? " " : "") + hex2(f[i]);
            }
            last += "   device " + hex2(f[0]) + ", command " + hex2(f[1]);
            if (const char* name = command_name(f[0], f[1])) {
                last += std::string(" (") + name + ")";
            }
            line(text, last);
        } else {
            line(text, "last_frame: none yet");
        }
        line(text, "reading: " + reading(s));
        return DiagResult::ok(std::move(text));
    }

    // The first thing that is wrong, in the order the signal travels.
    static std::string reading(const io::AtariSioStats& s)
    {
        if (s.cmd_edges == 0) {
            return "the CMD line has not changed since the last reset: the Atari is not sending commands, "
                   "or CMD is on the wrong pin or not connected";
        }
        if (s.capture.rx_bytes == 0) {
            return "CMD toggles but no bytes arrive on the SIO data-in pin: check that pin and the cable";
        }
        if (s.capture.frames == 0) {
            return "bytes arrive but no complete command frame: see the aborts (edge: CMD timing, "
                   "rx_error: baud or wiring, timeout/overflow: framing)";
        }
        if (s.router.disk_commands == 0 && s.router.nio_commands == 0) {
            return "command frames arrive, but none are for D1:-D8: or 0x7f (other devices only)";
        }
        if (s.router.disk_commands != 0) {
            return "disk commands reach the disk frontend; if the Atari still does not boot, "
                   "check the replies (the data-out pin) and disk.stats";
        }
        return "native (0x7f) commands reach the endpoint";
    }

    std::function<io::AtariSioStats()> _snapshot;
    std::function<void()> _reset;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_atari_sio_diagnostic_provider(
    std::function<io::AtariSioStats()> snapshot,
    std::function<void()> reset)
{
    if (!snapshot || !reset) {
        return nullptr;
    }
    return std::make_unique<AtariSioDiagnosticProvider>(std::move(snapshot), std::move(reset));
}

} // namespace fujinet::diag

#endif // FN_BUILD_ATARI_FUJIBUS_SIO || FN_PLATFORM_POSIX
