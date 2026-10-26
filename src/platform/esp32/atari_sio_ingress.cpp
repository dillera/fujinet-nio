#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)

#include "fujinet/platform/esp32/atari_sio_ingress.h"

#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/platform/esp32/uart_channel.h"

#include <algorithm>

extern "C" {
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    _owner_task = xTaskGetCurrentTaskHandle();
    _nio.set_clock([]() -> std::uint64_t { return static_cast<std::uint64_t>(esp_timer_get_time()); });
    const auto& pins = pinmap();
    configure_sio_lines(pins);
    _cmd_pin = pins.sio.cmd;
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
    // UartChannel routes UART TX to the data-in pin and owns it from here. Do not touch the pin with
    // gpio_set_direction(): in ESP-IDF that re-routes it to a plain GPIO output and the UART can
    // never transmit, so the Atari would never see an ACK. On the v1 hardware TX feeds a 74LS07
    // buffer input, which wants a driven high (push-pull), as the legacy firmware does.
    _uart = std::make_unique<UartChannel>(sio_config(config.channel.uart), pins.sio.uart);
    // From here the ingress answers commands. sio.status compares this with the first CMD edge.
    _reply.ready_us = static_cast<std::uint32_t>(esp_timer_get_time());
}

AtariSioIngress::~AtariSioIngress() = default;

bool AtariSioIngress::supports_work_wait() const { return _uart && _uart->supports_readable_wait(); }
bool AtariSioIngress::wait_for_work(std::chrono::milliseconds timeout)
{
    if (_capture.needs_immediate_poll()) {
        esp_rom_delay_us(100);
        return true;
    }
    // An ISR task notification wakes the owner on COMMAND even though the
    // UART queue has not yet received a byte. Cap the fallback to one tick.
    const TickType_t ticks = timeout.count() > 0 ? 1 : 0;
    return ulTaskNotifyTake(pdTRUE, ticks) != 0;
}

void AtariSioIngress::send(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty() || !_uart) return;
    // Timed before the write: this is when the reply starts, which is what the Atari waits for.
    _reply.record(static_cast<std::uint32_t>(esp_timer_get_time()), _capture.processing_start_us(),
                  _capture.processing_command(), _boundary.last_edge_us());
    _uart->write(bytes.data(), bytes.size());
    _uart->flushOutput();
}

io::AtariSioStats AtariSioIngress::stats() const
{
    io::AtariSioStats out;
    out.cmd_edges = _boundary.total_edges();
    out.first_edge_us = _boundary.first_edge_us();
    out.cmd_line_low = _cmd_pin >= 0 && _cmd_pin < GPIO_NUM_MAX &&
                       gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0;
    out.uart_rx_errors = _uart ? _uart->rx_errors() : 0;
    out.capture = _capture.stats();
    out.router = _router.stats();
    out.reply = _reply;
    out.poll_gap_us_max = _poll_gap_us_max;
    out.poll_gap_at_us = _poll_gap_at_us;
    out.stale_replies_dropped = _nio.stale_replies_dropped();
    return out;
}

void AtariSioIngress::reset_stats()
{
    _boundary.reset_total_edges();
    _capture.reset_stats();
    _router.reset_stats();
    _reply.reset();
    _poll_gap_us_max = 0;
    _poll_gap_at_us = 0;
}

void AtariSioIngress::note_cmd_edge() noexcept
{
    _boundary.note_cmd_edge(static_cast<std::uint32_t>(esp_timer_get_time()));
    BaseType_t woken = pdFALSE;
    if (_owner_task) vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(_owner_task), &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

void AtariSioIngress::poll()
{
    if (!_uart || _cmd_pin < 0 || _cmd_pin >= GPIO_NUM_MAX) return;
    const auto now = static_cast<std::uint32_t>(esp_timer_get_time());
    const auto edges = _boundary.total_edges();
    // Only a gap in which the Atari signalled CMD can have cost a command.
    if (_last_poll_us != 0 && edges != _edges_at_last_poll && now - _last_poll_us > _poll_gap_us_max) {
        _poll_gap_us_max = now - _last_poll_us;
        _poll_gap_at_us = _last_poll_us;
    }
    _last_poll_us = now;
    _edges_at_last_poll = edges;
    _capture.poll(_boundary, _router, *_uart,
                  [this] { return gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0; },
                  [] { return esp_timer_get_time(); });
}

} // namespace fujinet::platform::esp32

#endif
