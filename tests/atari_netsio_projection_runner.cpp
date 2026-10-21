// Real-core fixture route for the Story 2.3 virtual NetSIO projection.
//
// This is deliberately not a Q/W/P/R peer.  The production UDP NetSIO
// ingress owns the SIO transaction and dispatches a decoded FujiBus request
// through FujinetCore::routingManager().  The small device below supplies the
// fixed payload used by the independently literal core-dispatch fixture.

#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/platform/posix/atari_netsio_fujibus_channel.h"
#include "fujinet/platform/posix/udp_channel.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr const char* TAG = "atari-projection";
constexpr std::uint8_t fixture_device_id = 1;
std::atomic_bool stop_requested{false};

void request_stop(int)
{
    stop_requested.store(true);
}

class CoreDispatchFixtureDevice final : public fujinet::io::VirtualDevice {
public:
    fujinet::io::IOResponse handle(const fujinet::io::IORequest& request) override
    {
        // The literal raw fixture is decoded by the production shared codec
        // before it gets here.  Keep this guard so an accidental different
        // packet cannot look like a successful fixture execution.
        if (request.deviceId != fixture_device_id ||
            (request.command != 2 && request.command != 3) ||
            !request.params.empty() || !request.payload.empty()) {
            return fujinet::io::make_base_response(request, fujinet::io::StatusCode::InvalidRequest);
        }
        ++dispatches;
        FN_LOGI(TAG, "core-dispatch fixture route invocation %u", dispatches);
        if (dispatches > 2) {
            return fujinet::io::make_base_response(request, fujinet::io::StatusCode::DeviceBusy);
        }
        auto response = fujinet::io::make_success_response(request);
        if (request.command == 2) {
            response.payload = {0xaa, 0x55};
        } else {
            response.payload = {0x33, 0xcc};
        }
        return response;
    }

    std::uint32_t dispatches{0};
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
    (void)option_value(argc, argv, "--host", host);
    (void)option_value(argc, argv, "--port", port_text);

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

    fujinet::core::FujinetCore core;
    fujinet::core::register_disk_device(core);
    auto* disk = dynamic_cast<fujinet::io::DiskDevice*>(
        core.deviceManager().getDevice(0xfc));
    if (!disk) {
        FN_LOGE(TAG, "projection runner requires the real DiskDevice");
        return 1;
    }

    auto fixture = std::make_unique<CoreDispatchFixtureDevice>();
    if (!core.deviceManager().registerDevice(fixture_device_id, std::move(fixture))) {
        FN_LOGE(TAG, "failed to register core-dispatch fixture route");
        return 1;
    }

    auto udp = fujinet::platform::create_udp_channel(
        host, static_cast<std::uint16_t>(parsed_port));
    if (!udp) {
        FN_LOGE(TAG, "failed to create NetSIO UDP channel");
        return 1;
    }
    auto ingress = fujinet::platform::posix::create_atari_netsio_ingress(
        std::move(udp), disk->disk_service(), core.routingManager());

    std::cout << "Atari NetSIO projection initialized\n" << std::flush;
    while (!stop_requested.load()) {
        ingress->poll();
        core.tick();
        if (!ingress->wait_for_work(std::chrono::milliseconds(2))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return 0;
}
