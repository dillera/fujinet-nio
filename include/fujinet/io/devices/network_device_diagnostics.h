#pragma once

#include "fujinet/io/devices/network_device.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::io {

// Friend accessor for NetworkDevice internals, used by out-of-band diagnostics providers.
// Keeps diagnostic structures and helpers out of the main device header.
struct NetworkDeviceDiagnosticsAccessor {
    struct SessionRow {
        bool active{false};
        std::uint16_t handle{0};

        std::uint8_t method{0};
        std::uint8_t flags{0};
        bool completed{false};

        bool awaitingBody{false};
        std::uint32_t expectedBodyLen{0};
        std::uint32_t receivedBodyLen{0};

        std::uint64_t createdTick{0};
        std::uint64_t lastActivityTick{0};
        std::uint64_t idleMs{0};      // wall-clock time since the last request
        bool keepAlive{false};        // opened with the keep flag: never reaped for idleness

        std::string url;

        std::uint8_t translationType{0}; // ContentTranslationType
        std::string translationSelector;
        bool translationReady{false};
        std::uint64_t translatedSize{0};
    };

    static std::vector<SessionRow> sessions(const NetworkDevice& dev)
    {
        std::vector<SessionRow> out;
        out.reserve(NetworkDevice::MAX_SESSIONS);

        auto session_index = [&dev](const NetworkDevice::Session* s) -> std::uint8_t {
            return static_cast<std::uint8_t>(s - dev._sessions.data());
        };

        for (const auto& s : dev._sessions) {
            SessionRow row;
            row.active = s.active;
            if (s.active) {
                const std::uint8_t idx = session_index(&s);
                row.handle = NetworkDevice::make_handle(idx, s.generation);
                row.method = s.method;
                row.flags = s.flags;
                row.completed = s.completed;
                row.awaitingBody = s.awaitingBody;
                row.expectedBodyLen = s.expectedBodyLen;
                row.receivedBodyLen = s.receivedBodyLen;
                row.createdTick = s.createdTick;
                row.lastActivityTick = s.lastActivityTick;
                row.idleMs = dev.now_ms() - s.lastActivityMs;
                row.keepAlive = s.keepAlive;
                row.url = s.url;
                row.translationType = static_cast<std::uint8_t>(s.translation.type);
                row.translationSelector = s.translation.selector;
                row.translationReady = s.translationReady;
                row.translatedSize = s.translatedResultSize;
            }
            out.push_back(std::move(row));
        }

        return out;
    }

    static bool close(NetworkDevice& dev, std::uint16_t handle) noexcept
    {
        const auto idx = NetworkDevice::handle_index(handle);
        const auto gen = NetworkDevice::handle_generation(handle);
        if (idx >= NetworkDevice::MAX_SESSIONS) return false;

        auto& s = dev._sessions[idx];
        if (!s.active) return false;
        if (s.generation != gen) return false;

        dev.close_and_free(s);
        return true;
    }

    struct TranslationTotals {
        std::uint32_t count{0};
        std::uint32_t failures{0};
    };

    static TranslationTotals translation_totals(const NetworkDevice& dev) noexcept
    {
        return {dev._translationCount, dev._translationFailures};
    }

    // The logged translations, newest first.
    static std::vector<TranslationLogEntry> translation_log(const NetworkDevice& dev)
    {
        std::vector<TranslationLogEntry> out;
        const std::size_t n = dev._translationCount < NetworkDevice::TRANSLATION_LOG_SIZE
            ? dev._translationCount
            : NetworkDevice::TRANSLATION_LOG_SIZE;
        for (std::size_t i = 1; i <= n; ++i) {
            const std::size_t at = (dev._translationLogNext + NetworkDevice::TRANSLATION_LOG_SIZE - i)
                                 % NetworkDevice::TRANSLATION_LOG_SIZE;
            out.push_back(dev._translationLog[at]);
        }
        return out;
    }

    // Image translator pixel cap; a change applies to translations configured
    // afterwards (Open or TranslateConfigure), not to sessions already set up.
    static std::uint32_t image_max_pixels(const NetworkDevice& dev) noexcept
    {
        return dev._settings.imageMaxPixels;
    }

    static void set_image_max_pixels(NetworkDevice& dev, std::uint32_t maxPixels) noexcept
    {
        dev._settings.imageMaxPixels = maxPixels;
    }

    static std::size_t close_all(NetworkDevice& dev) noexcept
    {
        std::size_t n = 0;
        for (auto& s : dev._sessions) {
            if (!s.active) continue;
            dev.close_and_free(s);
            ++n;
        }
        return n;
    }
};

} // namespace fujinet::io


