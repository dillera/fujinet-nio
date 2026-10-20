#include "fujinet/io/transport/io_service.h"

namespace fujinet::io {

void IOService::serviceOnce()
{
    // Let each transport do any internal background work.
    for (auto* t : _transports) {
        if (t) {
            t->poll();
        }
    }

    // Process all available requests on each transport.
    for (auto* t : _transports) {
        if (!t) {
            continue;
        }

        IORequest req;
        while (t->receive(req)) {
            IOResponse resp = _handler.handleRequest(req);
            t->send(resp);
        }
    }
}

bool IOService::hasWaitableWorkSource() const
{
    for (auto* t : _transports) {
        if (t && t->supports_work_wait()) {
            return true;
        }
    }
    for (auto* source : _workSources) {
        if (source && source->supports_work_wait()) return true;
    }
    return false;
}

bool IOService::waitForWork(std::chrono::milliseconds timeout)
{
    for (auto* t : _transports) {
        if (t && t->supports_work_wait() && t->wait_for_work(std::chrono::milliseconds(0))) {
            return true;
        }
    }

    for (auto* source : _workSources) {
        if (source && source->supports_work_wait() && source->wait_for_work(std::chrono::milliseconds(0))) return true;
    }

    for (auto* t : _transports) {
        if (t && t->supports_work_wait()) {
            return t->wait_for_work(timeout);
        }
    }

    for (auto* source : _workSources) {
        if (source && source->supports_work_wait()) return source->wait_for_work(timeout);
    }

    return false;
}

} // namespace fujinet::io
