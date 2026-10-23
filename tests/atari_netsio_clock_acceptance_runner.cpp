// Real-core route for the Story 2.4 public-API Clock acceptance guest.
//
// The Atari guest talks only to public fujinet-nio-lib calls.  This process
// exposes the production UDP NetSIO ingress and AtariSioNioFrontend, forwarding
// every decoded FujiBus request to a real FujinetCore/ClockDevice.  The small
// forwarding observer is evidence-only; it neither owns SIO framing nor
// manufactures a protocol response.

#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"
#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/platform/posix/udp_channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fujinet::platform {
void set_test_unix_time_seconds(std::optional<std::uint64_t> frozen);
}

namespace {

constexpr const char* TAG = "atari-clock-acceptance";
constexpr std::uint64_t kClockUnixSeconds = 0x0102030405060708ULL;
constexpr std::string_view kNetworkFixtureUri = "fixture://nio-accept";
constexpr std::string_view kNetworkWriteMarker = "NET-WRITE-24";
constexpr std::string_view kNetworkReply = "NET-REPLY-24";
std::atomic_bool stop_requested{false};

void request_stop(int)
{
    stop_requested.store(true);
}

class CountingAcceptanceRoute final : public fujinet::io::IRequestHandler {
public:
    explicit CountingAcceptanceRoute(fujinet::io::IRequestHandler& target)
        : _target(target)
    {}

    fujinet::io::IOResponse handleRequest(const fujinet::io::IORequest& request) override
    {
        using fujinet::io::protocol::WireDeviceId;
        using fujinet::io::protocol::to_device_id;
        if (request.deviceId == to_device_id(WireDeviceId::Clock)) {
            ++_clock_dispatches;
            FN_LOGI(TAG, "ClockDevice routed request %u command=%u",
                    _clock_dispatches, static_cast<unsigned>(request.command));
        }
        if (request.deviceId == to_device_id(WireDeviceId::FileService)) {
            ++_file_dispatches;
            FN_LOGI(TAG, "FileDevice routed request %u command=%u",
                    _file_dispatches, static_cast<unsigned>(request.command));
        }
        if (request.deviceId == to_device_id(WireDeviceId::NetworkService)) {
            ++_network_dispatches;
            FN_LOGI(TAG, "NetworkDevice routed request %u command=%u",
                    _network_dispatches, static_cast<unsigned>(request.command));
        }
        return _target.handleRequest(request);
    }

private:
    fujinet::io::IRequestHandler& _target;
    std::uint32_t _clock_dispatches{};
    std::uint32_t _file_dispatches{};
    std::uint32_t _network_dispatches{};
};

struct NetworkFixtureState {
    std::uint32_t opens{};
    std::uint32_t writes{};
    std::uint32_t closes{};
    std::vector<std::uint8_t> last_write;
};

// This is a deterministic protocol backend registered with the production
// NetworkDevice. It is deliberately not a replacement device or a protocol
// transport: the normal NetworkDevice parses all requests, owns the handle,
// and dispatches to this implementation through ProtocolRegistry.
class AcceptanceNetworkProtocol final : public fujinet::io::INetworkProtocol {
public:
    explicit AcceptanceNetworkProtocol(std::shared_ptr<NetworkFixtureState> state)
        : _state(std::move(state))
    {}

    fujinet::io::StatusCode open(const fujinet::io::NetworkOpenRequest& request) override
    {
        ++_state->opens;
        FN_LOGI(TAG, "Network fixture open count=%u method=%u url=%s",
                _state->opens, static_cast<unsigned>(request.method), request.url.c_str());
        return request.method == 2 && request.url == kNetworkFixtureUri
            ? fujinet::io::StatusCode::Ok : fujinet::io::StatusCode::InvalidRequest;
    }

    fujinet::io::StatusCode write_body(std::uint32_t offset,
                                       const std::uint8_t* data,
                                       std::size_t length,
                                       std::uint16_t& written) override
    {
        ++_state->writes;
        _state->last_write.assign(data, data + length);
        const bool valid = offset == 0 && length == kNetworkWriteMarker.size() &&
            std::equal(_state->last_write.begin(), _state->last_write.end(),
                       kNetworkWriteMarker.begin());
        FN_LOGI(TAG, "Network fixture write count=%u bytes=%u valid=%u",
                _state->writes, static_cast<unsigned>(length), valid ? 1u : 0u);
        written = valid ? static_cast<std::uint16_t>(length) : 0;
        return valid ? fujinet::io::StatusCode::Ok : fujinet::io::StatusCode::InvalidRequest;
    }

    fujinet::io::StatusCode read_body(std::uint32_t offset,
                                      std::uint8_t* out,
                                      std::size_t capacity,
                                      std::uint16_t& read,
                                      bool& eof,
                                      bool& more_available) override
    {
        if (offset > kNetworkReply.size()) {
            return fujinet::io::StatusCode::InvalidRequest;
        }
        const std::size_t available = kNetworkReply.size() - static_cast<std::size_t>(offset);
        const std::size_t count = std::min(available, capacity);
        std::copy_n(reinterpret_cast<const std::uint8_t*>(kNetworkReply.data()) + offset,
                    count, out);
        read = static_cast<std::uint16_t>(count);
        eof = count == available;
        more_available = !eof;
        return fujinet::io::StatusCode::Ok;
    }

    fujinet::io::StatusCode info(fujinet::io::NetworkInfo& out) override
    {
        out = fujinet::io::NetworkInfo{};
        return fujinet::io::StatusCode::Ok;
    }

    void poll() override {}

    void close() override
    {
        ++_state->closes;
        FN_LOGI(TAG, "Network fixture close count=%u", _state->closes);
    }

private:
    std::shared_ptr<NetworkFixtureState> _state;
};

bool option_value(int argc, char** argv, std::string_view name, std::string& out)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == name && index + 1 < argc) {
            out = argv[++index];
            return true;
        }
        const std::string prefix = std::string(name) + "=";
        if (arg.starts_with(prefix)) {
            out = std::string(arg.substr(prefix.size()));
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    std::string port_text = "9997";
    std::string storage_root;
    (void)option_value(argc, argv, "--host", host);
    (void)option_value(argc, argv, "--port", port_text);
    (void)option_value(argc, argv, "--storage-root", storage_root);

    unsigned long parsed_port = 0;
    try {
        parsed_port = std::stoul(port_text);
    } catch (...) {
        std::cerr << "invalid --port: " << port_text << '\n';
        return 2;
    }
    if (parsed_port == 0 || parsed_port > 65535) {
        std::cerr << "invalid --port: " << port_text << '\n';
        return 2;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    fujinet::platform::set_test_unix_time_seconds(kClockUnixSeconds);
    fujinet::core::FujinetCore core;
    if (storage_root.empty()) {
        storage_root = (std::filesystem::current_path() / "fujinet-data").string();
    }
    std::error_code storage_error;
    std::filesystem::create_directories(storage_root, storage_error);
    if (storage_error) {
        FN_LOGE(TAG, "failed to create persistent FileDevice root: %s", storage_root.c_str());
        return 1;
    }
    auto host_filesystem = fujinet::platform::posix::create_host_filesystem(storage_root);
    if (!host_filesystem || !core.storageManager().registerFileSystem(std::move(host_filesystem))) {
        FN_LOGE(TAG, "failed to register persistent host filesystem");
        return 1;
    }
    fujinet::core::register_clock_device(core);
    fujinet::core::register_file_device(core);
    auto network_state = std::make_shared<NetworkFixtureState>();
    fujinet::io::ProtocolRegistry network_registry;
    if (!network_registry.register_scheme("fixture", [network_state] {
            return std::make_unique<AcceptanceNetworkProtocol>(network_state);
        })) {
        FN_LOGE(TAG, "failed to register deterministic NetworkDevice fixture protocol");
        return 1;
    }
    fujinet::core::register_network_device(core, std::move(network_registry), {});
    fujinet::core::register_disk_device(core);
    auto* disk = dynamic_cast<fujinet::io::DiskDevice*>(
        core.deviceManager().getDevice(
            fujinet::io::protocol::to_device_id(fujinet::io::protocol::WireDeviceId::DiskService)));
    if (!disk) {
        FN_LOGE(TAG, "Clock acceptance requires the real DiskDevice for the shared ingress");
        return 1;
    }

    CountingAcceptanceRoute route(core.routingManager());
    auto udp = fujinet::platform::create_udp_channel(host, static_cast<std::uint16_t>(parsed_port));
    if (!udp) {
        FN_LOGE(TAG, "failed to create NetSIO UDP channel");
        return 1;
    }
    auto ingress = fujinet::platform::posix::create_atari_netsio_ingress(
        std::move(udp), disk->disk_service(), route);

    std::cout << "Atari public-API Clock/File/Network acceptance initialized time=0807060504030201"
              << " storage=" << storage_root << '\n' << std::flush;
    while (!stop_requested.load()) {
        ingress->poll();
        core.tick();
        if (!ingress->wait_for_work(std::chrono::milliseconds(2))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return 0;
}
