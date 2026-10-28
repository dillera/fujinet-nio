#pragma once

#include "fujinet/io/core/client_notifier.h"
#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

// Abstract base for all addressable request handlers.
//
// Most handlers represent virtual devices (disk, printer, clock, modem, etc.).
// Some handlers are management services for FujiNet's own state. They still use
// the same transport routing and request lifecycle, but do not represent a
// device. Use VirtualService as a documentation alias for those handlers.
class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;

    // Handle a single request from the host.
    virtual IOResponse handle(const IORequest& request) = 0;
    virtual void resetClient(const void*) {}

    // Called periodically by IODeviceManager / IOService.
    // Devices that don't need polling can ignore this (default no-op).
    virtual void poll() {}

    // The manager that owns this device sets it when the device is registered.
    void set_client_notifier(IClientNotifier* notifier) noexcept { _notifier = notifier; }

protected:
    // Tell the client `owner` that a read it was refused may now succeed (see IClientNotifier).
    void notify_client_readable(const void* owner) const
    {
        if (_notifier && owner) _notifier->clientReadable(owner);
    }

private:
    IClientNotifier* _notifier{nullptr};
};

using VirtualService = VirtualDevice;

} // namespace fujinet::io
