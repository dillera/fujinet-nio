#include "fujinet/platform/channel_factory.h"
#include "fujinet/core/logging.h"

#include <memory>

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config.h"
#include "fujinet/platform/esp32/usb_cdc_channel.h"
#include "fujinet/platform/esp32/uart_channel.h"

extern "C" {
#include "sdkconfig.h"
}

namespace fujinet::platform {

static constexpr const char* TAG = "platform";

// A profile that asks for a packet link carries its packets over this stream.
static std::unique_ptr<io::Channel>
with_packet_link(const build::BuildProfile& profile, const config::FujiConfig& config,
                 std::unique_ptr<io::Channel> stream)
{
    const auto settings = io::packet_link_settings(
        config.channel.packetLink.capacity, config.channel.packetLink.recordTimeoutMs, false);
    if (profile.packetLink && stream) {
        FN_ELOG("Carrying FujiBus packets over a packet link (capacity %u, record timeout %u ms)",
                static_cast<unsigned>(settings.capacity),
                static_cast<unsigned>(settings.recordTimeoutMs));
    }
    return io::with_packet_link(profile.packetLink, std::move(stream), settings);
}

std::unique_ptr<fujinet::io::Channel>
create_channel_for_profile(const build::BuildProfile& profile, const config::FujiConfig& config,
                           disk::DiskService* /*disks*/)
{
    using build::ChannelKind;

    switch (profile.primaryChannel) {

    case ChannelKind::UsbCdcDevice:
#if CONFIG_TINYUSB_CDC_ENABLED && CONFIG_FN_FUJIBUS_TRANSPORT_USB_CDC
        FN_ELOG("Using TinyUSB CDC-ACM channel for UsbCdcDevice");
        return with_packet_link(profile, config, std::make_unique<esp32::UsbCdcChannel>());
#elif !CONFIG_TINYUSB_CDC_ENABLED
        FN_LOGE(TAG, "UsbCdcDevice selected but TinyUSB CDC is disabled in sdkconfig");
        return nullptr;
#else
        FN_LOGE(TAG, "UsbCdcDevice selected but FujiBus transport is not USB CDC");
        return nullptr;
#endif

    case ChannelKind::Pty:
        FN_LOGE(TAG, "Pty channel kind not supported on ESP32");
        return nullptr;

    case ChannelKind::TcpSocket:
        FN_LOGE(TAG, "TcpSocket channel kind not implemented on ESP32");
        return nullptr;

    case ChannelKind::UdpSocket:
        FN_LOGE(TAG, "UdpSocket channel kind not supported on ESP32");
        return nullptr;

    case ChannelKind::UartGpio:
        FN_ELOG("Using UartChannel for UartGpio (baud=%u, data_bits=%d)",
                static_cast<unsigned>(config.channel.uart.baudRate),
                config.channel.uart.dataBits);
        return with_packet_link(profile, config, std::make_unique<esp32::UartChannel>(config.channel.uart));

    case ChannelKind::SioGpio:
#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)
        // Atari SIO is not a stream Channel.  AtariSioIngress is constructed
        // after DiskDevice so it can directly own the shared DiskService.
        FN_ELOG("Atari SIO ingress owns SioGpio; no Channel is created");
        return nullptr;
#else
        FN_LOGE(TAG, "SioGpio selected but FN_BUILD_ATARI_FUJIBUS_SIO is not enabled");
        return nullptr;
#endif

    case ChannelKind::SerialPort:
        FN_LOGE(TAG, "SerialPort channel kind is POSIX-only; use UartGpio on ESP32");
        return nullptr;
    }

    FN_LOGE(TAG, "Unknown ChannelKind in create_channel_for_profile");
    return nullptr;
}

} // namespace fujinet::platform
