#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)

#include "fujinet/platform/esp32/atari_sio_ingress.h"

#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/platform/esp32/uart_channel.h"

extern "C" {
#include "driver/gpio.h"
}

namespace fujinet::platform::esp32 {
namespace {
constexpr std::uint32_t sio_baud = 19200;

config::UartConfig sio_config(config::UartConfig cfg)
{
    if (cfg.baudRate == config::UartConfig{}.baudRate) cfg.baudRate = sio_baud;
    cfg.dataBits = 8;
    cfg.parity = config::UartParity::None;
    cfg.stopBits = config::UartStopBits::One;
    cfg.flowControl = config::UartFlowControl::None;
    return cfg;
}

void configure_sio_lines(const PinMap& pins)
{
    for (const int pin : {pins.sio.cmd, pins.sio.mtr, pins.sio.cko}) {
        if (pin >= 0 && pin < GPIO_NUM_MAX) gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT);
    }
    for (const int pin : {pins.sio.int_pin, pins.sio.proc, pins.sio.cki}) {
        if (pin >= 0 && pin < GPIO_NUM_MAX) {
            gpio_set_level(static_cast<gpio_num_t>(pin), 1);
            gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT_OD);
        }
    }
}

void IRAM_ATTR cmd_edge_isr(void* arg)
{
    static_cast<AtariSioIngress*>(arg)->note_cmd_edge();
}
}

AtariSioIngress::AtariSioIngress(const config::FujiConfig& config,
                                 disk::DiskService& disks,
                                 io::IRequestHandler& routing)
    : _disk(disks)
    , _nio(routing)
    , _router(_disk, _nio, [this](const std::vector<std::uint8_t>& bytes) { send(bytes); })
{
    const auto& pins = pinmap();
    configure_sio_lines(pins);
    if (pins.sio.cmd >= 0 && pins.sio.cmd < GPIO_NUM_MAX) {
        const auto cmd = static_cast<gpio_num_t>(pins.sio.cmd);
        gpio_set_intr_type(cmd, GPIO_INTR_ANYEDGE);
        const esp_err_t service = gpio_install_isr_service(0);
        // The application may already own the global ISR service.  Handler
        // installation remains valid in that case and no second task/driver is
        // introduced.
        if ((service == ESP_OK || service == ESP_ERR_INVALID_STATE) &&
            gpio_isr_handler_add(cmd, cmd_edge_isr, this) == ESP_OK) {
            gpio_intr_enable(cmd);
        }
    }
    _uart = std::make_unique<UartChannel>(sio_config(config.channel.uart), pins.sio.uart);
    if (pins.sio.uart.tx >= 0 && pins.sio.uart.tx < GPIO_NUM_MAX) {
        gpio_set_level(static_cast<gpio_num_t>(pins.sio.uart.tx), 1);
        gpio_set_direction(static_cast<gpio_num_t>(pins.sio.uart.tx), GPIO_MODE_OUTPUT_OD);
    }
}

AtariSioIngress::~AtariSioIngress() = default;

bool AtariSioIngress::supports_work_wait() const { return _uart && _uart->supports_readable_wait(); }
bool AtariSioIngress::wait_for_work(std::chrono::milliseconds timeout) { return _uart && _uart->wait_for_readable(timeout); }

void AtariSioIngress::send(const std::vector<std::uint8_t>& bytes)
{
    if (!bytes.empty() && _uart) { _uart->write(bytes.data(), bytes.size()); _uart->flushOutput(); }
}

void AtariSioIngress::note_cmd_edge() noexcept
{
    _boundary.note_cmd_edge();
}

void AtariSioIngress::poll()
{
    if (!_uart) return;
    if (_boundary.consume()) _router.reset_command_collection();
    std::uint8_t bytes[256];
    while (_uart->available()) {
        const std::size_t count = _uart->read(bytes, sizeof(bytes));
        for (std::size_t i = 0; i < count; ++i) {
            // An edge can arrive while draining an event batch.  Bound the
            // next byte to the newly-recorded command transaction.
            if (_boundary.consume()) _router.reset_command_collection();
            _router.consume(bytes[i]);
        }
    }
}

} // namespace fujinet::platform::esp32

#endif
