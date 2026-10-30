#pragma once

#include <chrono>
#include <functional>
#include <vector>

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/devices/wifi_controller.h"
#include "fujinet/net/network_link.h"

namespace fujinet::io {

class WifiService final : public VirtualService {
public:
    using LinkProvider = std::function<net::INetworkLink*()>;
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    WifiService(config::FujiConfig& config,
                config::FujiConfigStore* store,
                LinkProvider linkProvider,
                Now now = [] { return Clock::now(); });

    IOResponse handle(const IORequest& request) override;
    void poll() override;
    std::uint8_t join_status() const { return static_cast<std::uint8_t>(_joinState); }

private:
    enum class JoinState : std::uint8_t { Idle, Pending, Connected, Failed };
    Now _now;
    JoinState _joinState{JoinState::Idle};
    config::WifiConfig _joinPrevious;
    config::WifiConfig _joinCandidate;
    std::chrono::steady_clock::time_point _joinStartAt{};
    std::chrono::steady_clock::time_point _joinDeadline{};
    bool _joinStartPending{false};
    WifiController _controller;
    std::vector<net::WifiScanRecord> _scanCache;
    bool _scanCacheValid{false};
};

} // namespace fujinet::io
