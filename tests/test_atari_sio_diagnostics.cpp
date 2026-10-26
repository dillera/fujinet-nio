#include "doctest.h"

#include "fujinet/diag/diagnostic_atari_sio.h"
#include "fujinet/io/atari_sio_stats.h"

#include <string>
#include <string_view>
#include <vector>

using fujinet::diag::DiagArgsView;
using fujinet::diag::DiagStatus;
using fujinet::io::AtariSioStats;

namespace {

fujinet::diag::DiagResult run(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    DiagArgsView args;
    args.argv = std::move(argv);
    return p.execute(args);
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

struct Fixture {
    AtariSioStats stats;
    int resets{0};
    std::unique_ptr<fujinet::diag::IDiagnosticProvider> provider = fujinet::diag::create_atari_sio_diagnostic_provider(
        [this] { return stats; }, [this] { ++resets; stats = {}; });
};

} // namespace

TEST_CASE("sio diagnostics need both a snapshot and a reset")
{
    CHECK(fujinet::diag::create_atari_sio_diagnostic_provider(nullptr, [] {}) == nullptr);
    CHECK(fujinet::diag::create_atari_sio_diagnostic_provider([] { return AtariSioStats{}; }, nullptr) == nullptr);
}

TEST_CASE("sio.status lists its commands and reports every counter")
{
    Fixture f;
    REQUIRE(f.provider != nullptr);
    CHECK(f.provider->provider_id() == "sio");
    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    f.provider->list_commands(cmds);
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0].name == "sio.status");
    CHECK(cmds[1].name == "sio.reset");

    f.stats.cmd_edges = 12;
    f.stats.cmd_line_low = true;
    f.stats.uart_rx_errors = 3;
    f.stats.capture = {.rx_bytes = 40, .frames = 7, .aborts_edge = 1, .aborts_timeout = 2, .aborts_overflow = 3, .aborts_rx_error = 4};
    f.stats.router.disk_commands = 5;
    f.stats.router.nio_commands = 1;
    f.stats.router.other_commands = 1;
    f.stats.router.has_last_frame = true;
    f.stats.router.last_frame = {0x31, 0x52, 0x01, 0x00, 0x84};

    const auto r = run(*f.provider, {"sio.status"});
    REQUIRE(r.status == DiagStatus::Ok);
    for (const char* expected : {"cmd_edges: 12", "low (asserted)", "rx_bytes: 40", "uart_rx_errors: 3", "frames: 7",
                                 "disk(D1:-D8:): 5", "nio(0x7f): 1", "other devices: 1", "edge=1", "timeout=2",
                                 "overflow=3", "rx_error=4", "0x31 0x52 0x01 0x00 0x84", "device 0x31", "(read sector)"}) {
        CAPTURE(expected);
        CHECK(contains(r.text, expected));
    }
}

TEST_CASE("sio.status shows an idle CMD line and no last frame before anything happens")
{
    Fixture f;
    const auto r = run(*f.provider, {"sio.status"});
    REQUIRE(r.status == DiagStatus::Ok);
    CHECK(contains(r.text, "high (idle)"));
    CHECK(contains(r.text, "last_frame: none yet"));
}

TEST_CASE("sio.status names the native commands and leaves unknown ones as hex")
{
    Fixture f;
    f.stats.router.has_last_frame = true;
    f.stats.router.last_frame = {0x7f, 'P', 0, 0, 0};
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "(poll)"));
    f.stats.router.last_frame = {0x31, 0x99, 0, 0, 0};
    const auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "command 0x99"));
    CHECK_FALSE(contains(text, "command 0x99 ("));
}

TEST_CASE("sio.status reading points at the first thing that is wrong, in signal order")
{
    Fixture f;
    auto reading = [&] { return run(*f.provider, {"sio.status"}).text; };

    CHECK(contains(reading(), "CMD line has not changed"));            // no edges at all

    f.stats.cmd_edges = 4;
    CHECK(contains(reading(), "no bytes arrive on the SIO data-in"));  // CMD toggles, silent UART

    f.stats.capture.rx_bytes = 20;
    CHECK(contains(reading(), "no complete command frame"));           // bytes but no frame

    f.stats.capture.frames = 3;
    f.stats.router.other_commands = 3;
    CHECK(contains(reading(), "other devices only"));                  // frames, none ours

    f.stats.router.disk_commands = 2;
    CHECK(contains(reading(), "disk commands reach the disk frontend"));

    f.stats.router.disk_commands = 0;
    f.stats.router.nio_commands = 1;
    CHECK(contains(reading(), "native (0x7f) commands reach the endpoint"));
}

TEST_CASE("sio.reset zeroes the counters through the ingress, and unknown commands are not ours")
{
    Fixture f;
    f.stats.capture.frames = 9;
    const auto r = run(*f.provider, {"sio.reset"});
    CHECK(r.status == DiagStatus::Ok);
    CHECK(f.resets == 1);
    CHECK(f.stats.capture.frames == 0);
    CHECK(run(*f.provider, {"sio.nonsense"}).status == DiagStatus::NotFound);
}

TEST_CASE("sio.status shows when the ingress was ready and when the Atari first spoke")
{
    Fixture f;
    f.stats.reply.ready_us = 2'235'000;
    f.stats.first_edge_us = 2'790'000;
    f.stats.capture.first_frame_us = 2'802'000;
    auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "ingress ready 2235   first CMD edge 2790   first frame 2802"));
    CHECK_FALSE(contains(text, "watch:"));    // ready before the Atari spoke: nothing to flag

    f.stats = {};
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "ingress ready none   first CMD edge none   first frame none"));
}

TEST_CASE("sio.status reports the reaction, reply and handler times in milliseconds")
{
    Fixture f;
    f.stats.capture.lag_us_max = 400;
    f.stats.reply.replies = 122;
    f.stats.reply.reply_us_last = 1200;
    f.stats.reply.reply_us_max = 3100;
    f.stats.reply.handle_us_last = 900;
    f.stats.reply.handle_us_max = 2800;
    const auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "react: CMD release -> start of processing, max 0.4 ms"));
    CHECK(contains(text, "reply: CMD release -> first byte, last 1.2 ms, max 3.1 ms, over 16 ms: 0   (replies 122)"));
    CHECK(contains(text, "handler: last 0.9 ms, max 2.8 ms"));
    CHECK_FALSE(contains(text, "watch:"));
}

TEST_CASE("sio.status warns about late ACKs, slow handlers and a first edge before the ingress was ready")
{
    Fixture f;
    f.stats.reply.reply_over_deadline = 2;
    f.stats.reply.reply_us_max = 21'500;
    auto text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "watch: 2 command reply(ies) started more than 16 ms after CMD was released (worst 21.5 ms)"));

    f.stats = {};
    f.stats.reply.handle_us_max = 800'000;
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "watch: a request spent 800.0 ms in its handler"));
    CHECK(contains(text, "leaves the native endpoint stuck"));

    f.stats = {};
    f.stats.reply.handle_us_max = 99'999;      // just under the limit: no warning
    CHECK_FALSE(contains(run(*f.provider, {"sio.status"}).text, "watch:"));

    f.stats = {};
    f.stats.poll_gap_us_max = 1'350'000;
    f.stats.poll_gap_at_us = 1'650'000;
    text = run(*f.provider, {"sio.status"}).text;
    CHECK(contains(text, "while CMD was active, 1350.0 ms, starting at 1650"));
    CHECK(contains(text, "watch: the SIO task went 1350.0 ms without polling while CMD was active"));

    f.stats = {};
    f.stats.stale_replies_dropped = 3;
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "native replies never read, discarded: 3"));
    f.stats.stale_replies_dropped = 0;
    CHECK_FALSE(contains(run(*f.provider, {"sio.status"}).text, "never read"));

    f.stats = {};
    f.stats.reply.ready_us = 2'000'000;
    f.stats.first_edge_us = 1'900'000;
    CHECK(contains(run(*f.provider, {"sio.status"}).text, "watch: the first CMD edge came before the ingress was ready"));
}
