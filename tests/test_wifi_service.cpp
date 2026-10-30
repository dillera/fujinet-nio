#include "doctest.h"

#include "fujinet/core/core.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/wifi_service.h"
#include "fujinet/platform/posix/wifi_link.h"

#include <memory>
#include <vector>

namespace {
struct Store final : fujinet::config::FujiConfigStore {
    int saves{0};
    fujinet::config::FujiConfig load() override { return {}; }
    void save(const fujinet::config::FujiConfig&) override { ++saves; }
};

struct Link final : fujinet::net::INetworkLink {
    fujinet::net::LinkState state_value{fujinet::net::LinkState::Connected};
    bool connected{false};
    bool connect_starts{false};
    bool active_available{false};
    fujinet::net::WifiCredentials active;
    bool active_credentials(fujinet::net::WifiCredentials& out) const override {
        if (!active_available) return false;
        out = active;
        return true;
    }
    std::vector<std::pair<std::string, std::string>> attempts;
    bool scan_enabled{false};
    int scans{0};
    std::string bssid;
    fujinet::net::WifiBssid mac{{0x24, 0x6f, 0x28, 0x01, 0x02, 0x03}, true};
    fujinet::net::LinkState state() const override { return state_value; }
    fujinet::net::WifiBssid mac_address() const override { return mac; }
    void connect(std::string ssid, std::string pass) override {
        connected = true;
        attempts.emplace_back(std::move(ssid), std::move(pass));
        if (connect_starts) state_value = fujinet::net::LinkState::Connecting;
    }
    void disconnect() override { connected = false; }
    void poll() override {}
    std::string ip_address() const override { return "192.0.2.2"; }
    std::string subnet_mask() const override { return "255.255.255.0"; }
    std::string gateway() const override { return "192.0.2.1"; }
    std::string dns_server() const override { return "192.0.2.1"; }
    fujinet::net::WifiLinkCapabilities capabilities() const override {
        auto flags = static_cast<std::uint16_t>(fujinet::net::WifiCapabilityConfig |
                                            fujinet::net::WifiCapabilityStatus |
                                            fujinet::net::WifiCapabilityConnect |
                                            fujinet::net::WifiCapabilityDisconnect |
                                            fujinet::net::WifiCapabilityBssidSelect);
        if (scan_enabled) flags |= fujinet::net::WifiCapabilityScan;
        return {flags,
                fujinet::net::WifiBackendKind::Esp32};
    }
    void set_bssid(std::string value) override { bssid = std::move(value); }
    bool supports_scan() const override { return scan_enabled; }
    fujinet::net::WifiScanResult scan_wifi() override {
        ++scans;
        fujinet::net::WifiScanResult result;
        result.success = scan_enabled;
        for (const char* ssid : {"one", "two", "three"}) {
            fujinet::net::WifiScanRecord record;
            record.ssid = ssid;
            result.records.push_back(record);
        }
        return result;
    }
};
}

TEST_CASE("wifi service never returns password") {
    fujinet::config::FujiConfig config;
    config.wifi.enabled = true;
    config.wifi.ssid = "test-net";
    config.wifi.passphrase = "secret";
    Store store;
    Link link;
    fujinet::io::WifiService service(config, &store, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x02;
    request.payload = {1};
    const auto response = service.handle(request);
    CHECK(response.status == fujinet::io::StatusCode::Ok);
    CHECK(std::string(response.payload.begin(), response.payload.end()).find("secret") == std::string::npos);
    CHECK(response.payload[2] == 1);
}

TEST_CASE("wifi service validates and applies write-only config") {
    fujinet::config::FujiConfig config;
    Store store;
    Link link;
    fujinet::io::WifiService service(config, &store, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x03;
    request.payload = {1, 0x3f, 1, 4, 'n', 'e', 't', '1', 17,
                       'a','a',':','b','b',':','c','c',':','d','d',':','e','e',':','f','f',
                       6, 's','e','c','r','e','t'};
    const auto response = service.handle(request);
    CHECK(response.status == fujinet::io::StatusCode::Ok);
    CHECK(config.wifi.ssid == "net1");
    CHECK(config.wifi.bssid == "aa:bb:cc:dd:ee:ff");
    CHECK(config.wifi.passphrase == "secret");
    CHECK(store.saves == 1);
    CHECK(link.connected);
    CHECK(link.bssid == config.wifi.bssid);
}

TEST_CASE("wifi service rejects malformed requests") {
    fujinet::config::FujiConfig config;
    fujinet::io::WifiService service(config, nullptr, {});
    fujinet::io::IORequest request;
    request.command = 0x02;
    request.payload = {2};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
    request.payload = {1, 0};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
}

TEST_CASE("wifi service caches scans between pages") {
    fujinet::config::FujiConfig config;
    Link link;
    link.scan_enabled = true;
    fujinet::io::WifiService service(config, nullptr, [&] { return &link; });

    fujinet::io::IORequest request;
    request.command = 0x04;
    request.payload = {1, 0, 0, 2};
    auto first = service.handle(request);
    CHECK(first.status == fujinet::io::StatusCode::Ok);
    CHECK(first.payload[2] == 2);
    CHECK(first.payload[1] == 1);
    CHECK(link.scans == 1);

    request.payload = {1, 1, 0, 2};
    auto second = service.handle(request);
    CHECK(second.status == fujinet::io::StatusCode::Ok);
    CHECK(second.payload[2] == 2);
    CHECK(second.payload[1] == 0);
    CHECK(link.scans == 1);

    request.payload = {1, 0, 0, 1};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::Ok);
    CHECK(link.scans == 2);
}

TEST_CASE("wifi service reports the adapter MAC") {
    fujinet::config::FujiConfig config;
    Link link;
    fujinet::io::WifiService service(config, nullptr, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x05;
    request.payload = {1};
    const auto response = service.handle(request);
    REQUIRE(response.status == fujinet::io::StatusCode::Ok);
    CHECK(response.payload == std::vector<std::uint8_t>{1, 1, 0x24, 0x6f, 0x28, 0x01, 0x02, 0x03});

    // Newer clients get the version-1 reply; unknown trailing bytes are ignored.
    for (const std::vector<std::uint8_t>& payload : {std::vector<std::uint8_t>{2},
                                                     std::vector<std::uint8_t>{1, 0}}) {
        request.payload = payload;
        const auto newer = service.handle(request);
        CHECK(newer.status == fujinet::io::StatusCode::Ok);
        CHECK(newer.payload == response.payload);
    }
    for (const std::vector<std::uint8_t>& payload : {std::vector<std::uint8_t>{},
                                                     std::vector<std::uint8_t>{0}}) {
        request.payload = payload;
        CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
    }
}

TEST_CASE("wifi service's older commands still need exactly version 1") {
    fujinet::config::FujiConfig config;
    Link link;
    fujinet::io::WifiService service(config, nullptr, [&] { return &link; });
    fujinet::io::IORequest request;
    request.command = 0x01; // GET_STATUS
    request.payload = {2};
    CHECK(service.handle(request).status == fujinet::io::StatusCode::InvalidRequest);
}

TEST_CASE("wifi service reports no MAC without a link") {
    fujinet::config::FujiConfig config;
    fujinet::io::WifiService service(config, nullptr, [] { return nullptr; });
    fujinet::io::IORequest request;
    request.command = 0x05;
    request.payload = {1};
    const auto response = service.handle(request);
    REQUIRE(response.status == fujinet::io::StatusCode::Ok);
    CHECK(response.payload == std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("a MAC address prints in canonical form") {
    fujinet::net::MacAddress mac{{0x02, 0x00, 0xAB, 0x0C, 0xFF, 0xF0}, true};
    CHECK(fujinet::net::to_string(mac) == "02:00:ab:0c:ff:f0");
    mac.valid = false;
    CHECK(fujinet::net::to_string(mac).empty());
}

TEST_CASE("net.wifi.status shows the adapter MAC") {
    fujinet::core::FujinetCore core;
    fujinet::io::FujiDevice fuji(nullptr, nullptr);
    Link link;
    auto ctx = std::make_shared<fujinet::diag::NetworkDiagWifiContext>();
    ctx->fuji = &fuji;
    ctx->ensure_wifi = [&]() -> fujinet::net::INetworkLink* { return &link; };
    auto diag = fujinet::diag::create_network_diagnostic_provider(core, ctx);
    REQUIRE(diag != nullptr);

    fujinet::diag::DiagArgsView args;
    args.argv = {"net.wifi.status"};
    auto r = diag->execute(args);
    REQUIRE(r.status == fujinet::diag::DiagStatus::Ok);
    CHECK(r.text.find("mac: 24:6f:28:01:02:03") != std::string::npos);

    link.mac.valid = false;
    r = diag->execute(args);
    CHECK(r.text.find("mac: unavailable") != std::string::npos);
}

TEST_CASE("POSIX Wi-Fi backends advertise real capabilities") {
    using fujinet::platform::posix::PosixWifiLink;
    using fujinet::platform::posix::WifiBackendMode;

    PosixWifiLink simulated(WifiBackendMode::Simulated);
    CHECK((simulated.capabilities().flags & fujinet::net::WifiCapabilitySimulated) != 0);
    CHECK(simulated.supports_scan());
    simulated.connect("FujiNet-Sim", "secret");
    CHECK(simulated.state() == fujinet::net::LinkState::Connected);
    CHECK(simulated.scan_wifi().records.size() == 3);
    CHECK(simulated.mac_address().valid);

    PosixWifiLink unavailable(WifiBackendMode::Unavailable);
    CHECK(!unavailable.supports_scan());
    CHECK(unavailable.capabilities().backend == fujinet::net::WifiBackendKind::Unavailable);
    unavailable.connect("ssid", "password");
    CHECK(unavailable.state() == fujinet::net::LinkState::Failed);
    CHECK(!unavailable.mac_address().valid);
}

TEST_CASE("wifi trial commits only after connection and rolls back on failure or timeout") {
    using namespace fujinet;
    config::FujiConfig config;
    config.wifi.enabled = true;
    config.wifi.ssid = "old";
    config.wifi.passphrase = "old-pass";
    config.wifi.bssid = "aa:bb:cc:dd:ee:ff";
    Store store;
    Link link;
    link.connect_starts = true;
    auto now = io::WifiService::Clock::time_point{};
    io::WifiService service(config, &store, [&] { return &link; }, [&] { return now; });
    io::IORequest req;
    req.command = 6;
    req.payload = {1, 3, 'n', 'e', 'w', 3, 'b', 'a', 'd'};
    REQUIRE(service.handle(req).status == io::StatusCode::Ok);
    CHECK(store.saves == 0);
    CHECK(config.wifi.ssid == "old");
    CHECK(link.attempts.empty());
    CHECK(service.handle(req).status == io::StatusCode::DeviceBusy);
    service.poll();
    CHECK(link.attempts.empty());
    now += std::chrono::milliseconds(500);
    service.poll();
    REQUIRE(link.attempts.size() == 1);
    CHECK(link.attempts.back() == std::make_pair(std::string("new"), std::string("bad")));
    CHECK(link.bssid.empty());
    link.state_value = net::LinkState::Failed;
    service.poll();
    REQUIRE(link.attempts.size() == 2);
    CHECK(link.attempts.back() == std::make_pair(std::string("old"), std::string("old-pass")));
    CHECK(link.bssid == "aa:bb:cc:dd:ee:ff");
    CHECK(config.wifi.ssid == "old");
    CHECK(store.saves == 0);
    req.command = 7; req.payload = {1};
    CHECK(service.handle(req).payload == std::vector<std::uint8_t>{1, 3});

    req.command = 6; req.payload = {1, 3, 'n', 'e', 'w', 4, 'g', 'o', 'o', 'd'};
    REQUIRE(service.handle(req).status == io::StatusCode::Ok);
    now += std::chrono::milliseconds(500);
    service.poll();
    link.state_value = net::LinkState::Connected;
    service.poll();
    CHECK(config.wifi.ssid == "new");
    CHECK(config.wifi.passphrase == "good");
    CHECK(config.wifi.bssid.empty());
    CHECK(store.saves == 1);
    req.command = 7; req.payload = {1, 99}; // trailing extensions are ignored
    CHECK(service.handle(req).payload == std::vector<std::uint8_t>{1, 2});

    // A wrong password for the same SSID must not be mistaken for success.
    req.command = 6; req.payload = {1, 3, 'n', 'e', 'w', 3, 'b', 'a', 'd'};
    REQUIRE(service.handle(req).status == io::StatusCode::Ok);
    now += std::chrono::milliseconds(500);
    service.poll();
    link.state_value = net::LinkState::Failed;
    service.poll();
    CHECK(config.wifi.ssid == "new");
    CHECK(config.wifi.passphrase == "good");
    CHECK(store.saves == 1);
    CHECK(link.attempts.back() == std::make_pair(std::string("new"), std::string("good")));
    req.command = 7; req.payload = {1};
    CHECK(service.handle(req).payload == std::vector<std::uint8_t>{1, 3});

    req.command = 6; req.payload = {1, 3, 'b', 'a', 'd', 0};
    REQUIRE(service.handle(req).status == io::StatusCode::Ok);
    now += std::chrono::milliseconds(500);
    service.poll();
    now += std::chrono::seconds(31);
    service.poll();
    CHECK(config.wifi.ssid == "new");
    CHECK(store.saves == 1);
    CHECK(link.attempts.back() == std::make_pair(std::string("new"), std::string("good")));
    req.command = 7; req.payload = {1};
    CHECK(service.handle(req).payload == std::vector<std::uint8_t>{1, 3});
}

TEST_CASE("wifi trial requires valid input, a writable store and a capable link") {
    using namespace fujinet;
    config::FujiConfig config;
    Link link;
    io::IORequest req; req.command = 6; req.payload = {1, 0, 0};
    io::WifiService missingStore(config, nullptr, [&] { return &link; });
    CHECK(missingStore.handle(req).status == io::StatusCode::InvalidRequest);
    req.payload = {1, 1, 'x', 0};
    CHECK(missingStore.handle(req).status == io::StatusCode::NotReady);
    Store store;
    io::WifiService missingLink(config, &store, [] { return nullptr; });
    CHECK(missingLink.handle(req).status == io::StatusCode::NotReady);
    CHECK(config.wifi.ssid.empty());
    CHECK(store.saves == 0);
}

TEST_CASE("net.wifi.status reports trial state and prevents disruptive commands") {
    using namespace fujinet;
    core::FujinetCore core;
    io::FujiDevice fuji(nullptr, nullptr);
    fuji.config_mut().wifi.enabled = true;
    fuji.config_mut().wifi.ssid = "old";
    fuji.config_mut().wifi.passphrase = "old-secret";
    Store store;
    Link link;
    link.connect_starts = true;
    auto now = io::WifiService::Clock::time_point{};
    auto service = std::make_unique<io::WifiService>(
        fuji.config_mut(), &store, [&] { return &link; }, [&] { return now; });
    auto* trial = service.get();
    REQUIRE(core.deviceManager().registerDevice(0xF3, std::move(service)));
    auto ctx = std::make_shared<diag::NetworkDiagWifiContext>();
    ctx->fuji = &fuji;
    ctx->ensure_wifi = [&]() -> net::INetworkLink* { return &link; };
    auto console = diag::create_network_diagnostic_provider(core, ctx);
    diag::DiagArgsView status; status.argv = {"net.wifi.status"};
    CHECK(console->execute(status).text.find("join_trial: idle") != std::string::npos);
    io::IORequest req; req.command = 6; req.payload = {1, 3, 'n','e','w', 3, 'b','a','d'};
    REQUIRE(trial->handle(req).status == io::StatusCode::Ok);
    CHECK(console->execute(status).text.find("join_trial: connecting") != std::string::npos);
    diag::DiagArgsView set; set.argv = {"net.wifi.set", "ssid", "other"};
    CHECK(console->execute(set).status == diag::DiagStatus::Busy);
    diag::DiagArgsView scan; scan.argv = {"net.wifi.scan"};
    CHECK(console->execute(scan).status == diag::DiagStatus::Busy);
    req.command = 4; req.payload = {1, 0, 0, 1};
    CHECK(trial->handle(req).status == io::StatusCode::DeviceBusy);
    now += std::chrono::milliseconds(500);
    trial->poll();
    link.state_value = net::LinkState::Failed;
    trial->poll();
    const auto result = console->execute(status);
    CHECK(result.text.find("join_trial: failed; previous network restore attempted") != std::string::npos);
    CHECK(result.text.find("old-secret") == std::string::npos);
    CHECK(result.text.find("bad") == std::string::npos);
}

TEST_CASE("wifi trial recovers the live network when saved credentials differ") {
    using namespace fujinet;
    config::FujiConfig config;
    config.wifi.enabled = true;
    config.wifi.ssid = "incorrectly-saved";
    config.wifi.passphrase = "bad-saved";
    Store store;
    Link link;
    link.connect_starts = true;
    link.active_available = true;
    link.active = {"live", "live-pass", "aa:bb:cc:dd:ee:ff"};
    auto now = io::WifiService::Clock::time_point{};
    io::WifiService service(config, &store, [&] { return &link; }, [&] { return now; });
    io::IORequest req; req.command = 6; req.payload = {1, 3, 'n','e','w', 3, 'b','a','d'};
    REQUIRE(service.handle(req).status == io::StatusCode::Ok);
    CHECK(config.wifi.ssid == "live");
    CHECK(config.wifi.passphrase == "live-pass");
    CHECK(config.wifi.bssid == "aa:bb:cc:dd:ee:ff");
    CHECK(store.saves == 1);
    now += std::chrono::milliseconds(500);
    service.poll();
    link.state_value = net::LinkState::Failed;
    service.poll();
    REQUIRE(link.attempts.size() == 2);
    CHECK(link.attempts.back() == std::make_pair(std::string("live"), std::string("live-pass")));
    CHECK(link.bssid == "aa:bb:cc:dd:ee:ff");
    CHECK(config.wifi.ssid == "live");
    req.command = 7; req.payload = {1};
    CHECK(service.handle(req).payload == std::vector<std::uint8_t>{1, 3});
}
