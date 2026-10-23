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
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"
#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/platform/posix/udp_channel.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace fujinet::platform {
void set_test_unix_time_seconds(std::optional<std::uint64_t> frozen);
}

namespace {

constexpr const char* TAG = "atari-clock-acceptance";
constexpr std::uint64_t kClockUnixSeconds = 0x0102030405060708ULL;
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
        return _target.handleRequest(request);
    }

private:
    fujinet::io::IRequestHandler& _target;
    std::uint32_t _clock_dispatches{};
    std::uint32_t _file_dispatches{};
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

    std::cout << "Atari public-API Clock/File acceptance initialized time=0807060504030201"
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
