#include "doctest.h"
#include "fujinet/platform/esp32/led_manager.h"

TEST_CASE("network activity holds the LED briefly and coalesces transfers")
{
    fujinet::platform::esp32::NetworkActivityPulse pulse;
    CHECK_FALSE(pulse.tick(0));
    CHECK(pulse.tick(1));
    CHECK(pulse.tick(1));
    CHECK(pulse.tick(1));
    CHECK(pulse.tick(2)); // another transfer extends the pulse
    CHECK(pulse.tick(2));
    CHECK(pulse.tick(2));
    CHECK(pulse.tick(2));
    CHECK_FALSE(pulse.tick(2));
    CHECK(pulse.tick(3));
}
