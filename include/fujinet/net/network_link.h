#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace fujinet::net {

enum class LinkState {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

struct MacAddress {
    std::uint8_t bytes[6]{};
    bool valid{false};
};

// An access point's MAC address.
using WifiBssid = MacAddress;

// Canonical text form, "aa:bb:cc:dd:ee:ff"; empty when not valid.
inline std::string to_string(const MacAddress& mac)
{
    if (!mac.valid) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(17);
    for (std::size_t i = 0; i < 6; ++i) {
        if (i) text += ':';
        text += hex[mac.bytes[i] >> 4];
        text += hex[mac.bytes[i] & 0x0F];
    }
    return text;
}

struct WifiScanRecord {
    std::string ssid;
    WifiBssid bssid;
    std::int8_t rssi{0};
    std::uint8_t channel{0};
    std::uint8_t auth{0};
};

struct WifiScanResult {
    std::vector<WifiScanRecord> records;
    bool success{false};
};

enum class WifiBackendKind : std::uint8_t {
    Unavailable = 0,
    Esp32 = 1,
    PosixHost = 2,
    PosixSimulated = 3,
};

enum WifiCapability : std::uint16_t {
    WifiCapabilityConfig = 1U << 0,
    WifiCapabilityStatus = 1U << 1,
    WifiCapabilityConnect = 1U << 2,
    WifiCapabilityDisconnect = 1U << 3,
    WifiCapabilityScan = 1U << 4,
    WifiCapabilityBssidSelect = 1U << 5,
    WifiCapabilityHostManaged = 1U << 6,
    WifiCapabilitySimulated = 1U << 7,
};

// Internal-only snapshot; never encoded in status or diagnostics.
struct WifiCredentials {
    std::string ssid;
    std::string password;
    std::string bssid;
};

struct WifiLinkCapabilities {
    std::uint16_t flags{WifiCapabilityConfig | WifiCapabilityStatus};
    WifiBackendKind backend{WifiBackendKind::Unavailable};
};

class INetworkLink {
public:
    virtual ~INetworkLink() = default;

    virtual LinkState state() const = 0;

    virtual void connect(std::string ssid, std::string pass) = 0;
    virtual void set_bssid(std::string) {}
    virtual void disconnect() = 0;

    // Called from core tick (ESP32 can drive state machine here).
    virtual void poll() = 0;

    virtual std::string ip_address() const = 0;

    virtual WifiBssid current_bssid() const { return {}; }
    // Available only when the backend knows the credentials of a live link.
    virtual bool active_credentials(WifiCredentials&) const { return false; }
    // Station interface MAC address; valid=false when the backend cannot report it.
    virtual MacAddress mac_address() const { return {}; }
    virtual std::int8_t rssi() const { return 0; }
    virtual std::string subnet_mask() const { return {}; }
    virtual std::string gateway() const { return {}; }
    virtual std::string dns_server() const { return {}; }
    virtual WifiLinkCapabilities capabilities() const { return {}; }
    virtual bool supports_scan() const { return false; }
    virtual WifiScanResult scan_wifi() { return {}; }
};

}
