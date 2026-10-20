#pragma once

#include <chrono>

namespace fujinet::io {

// Work which must wake the core loop but is not an I/O transport.  This keeps
// physical protocol ingress separate from FujiBus request routing.
class IWorkSource {
public:
    virtual ~IWorkSource() = default;
    virtual bool supports_work_wait() const = 0;
    virtual bool wait_for_work(std::chrono::milliseconds timeout) = 0;
};

} // namespace fujinet::io
