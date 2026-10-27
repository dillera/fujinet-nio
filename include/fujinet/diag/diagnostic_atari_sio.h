#pragma once

// Atari-only diagnostics, kept out of diagnostic_provider.h so other builds never see them.
// The implementation compiles only for the physical SIO build (FN_BUILD_ATARI_FUJIBUS_SIO)
// and for host tests.

#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/atari_sio_stats.h"

#include <functional>
#include <memory>

namespace fujinet::diag {

// Atari SIO ingress bring-up (sio.status, sio.reset): CMD-line and UART activity, frames, aborts
// and the last command seen, with a plain-language reading of them. `snapshot` gathers the
// counters (the ingress adds its hardware parts) and `reset` zeroes them. `probe_baud` (optional)
// sets the SIO UART to a baud, returns what the hardware reports and restores it, for sio.baud, which
// compares what the UART can do with the POKEY high-speed rates (see docs/atari/hsio.md).
std::unique_ptr<IDiagnosticProvider> create_atari_sio_diagnostic_provider(
    std::function<::fujinet::io::AtariSioStats()> snapshot,
    std::function<void()> reset,
    std::function<std::uint32_t(std::uint32_t)> probe_baud = {});

} // namespace fujinet::diag
