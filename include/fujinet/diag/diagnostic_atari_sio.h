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
// counters (the ingress adds its hardware parts) and `reset` zeroes them.
std::unique_ptr<IDiagnosticProvider> create_atari_sio_diagnostic_provider(
    std::function<::fujinet::io::AtariSioStats()> snapshot,
    std::function<void()> reset);

} // namespace fujinet::diag
