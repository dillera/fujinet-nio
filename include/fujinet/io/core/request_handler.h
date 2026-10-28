// File: io/core/request_handler.h
#pragma once

#include <functional>

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

class IRequestHandler {
public:
    virtual ~IRequestHandler() = default;

    // Handle a single request and return a response.
    // Implementations may route to devices, modes, etc.
    virtual IOResponse handleRequest(const IORequest& request) = 0;
    virtual void resetClient(DeviceID, const void*) {}
    // Call `readable` when a device tells the client `owner` that a request it was refused may now
    // succeed (IClientNotifier). A handler that has no devices ignores this.
    virtual void watchClient(const void*, std::function<void()>) {}
};

} // namespace fujinet::io
