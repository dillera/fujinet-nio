#pragma once

#include <atomic>

namespace fujinet::io {

// Kept independent of ESP-IDF so transaction reset behaviour can be tested on
// the host.  The GPIO ISR only records an edge; the main-loop ingress performs
// the reset before consuming UART bytes.
class AtariSioTransactionBoundary {
public:
    void note_cmd_edge() noexcept { _pending.store(true, std::memory_order_release); }
    bool consume() noexcept { return _pending.exchange(false, std::memory_order_acq_rel); }

private:
    std::atomic_bool _pending{false};
};

} // namespace fujinet::io
