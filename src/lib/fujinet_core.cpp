#include "fujinet/core/core.h"

namespace fujinet::core {

FujinetCore::FujinetCore()
    : _deviceManager()
    , _routing(_deviceManager)
    , _ioService(_routing)
    , _storageManager()
{
}

void FujinetCore::tick()
{
    // 1. Let transports process I/O.
    _ioService.serviceOnce();

    // 2. Let devices do background work.
    _deviceManager.pollDevices();

    // 3. Increment tick counter for diagnostics.
    ++_tickCount;
}

bool FujinetCore::hasWaitableWorkSource() const
{
    return _ioService.hasWaitableWorkSource();
}

bool FujinetCore::waitForWork(std::chrono::milliseconds timeout)
{
    return _ioService.waitForWork(timeout);
}

void FujinetCore::addTransport(io::ITransport* transport)
{
    _ioService.addTransport(transport);
}

void FujinetCore::addWorkSource(io::IWorkSource* source)
{
    _ioService.addWorkSource(source);
}

} // namespace fujinet::core
