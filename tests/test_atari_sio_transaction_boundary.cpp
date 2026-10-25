#include "doctest.h"

#include "fujinet/io/atari_sio_transaction_boundary.h"

TEST_CASE("Atari SIO CMD edge records one host-testable transaction boundary")
{
    fujinet::io::AtariSioTransactionBoundary boundary;
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    CHECK(boundary.consume());
    CHECK_FALSE(boundary.consume());
    boundary.note_cmd_edge();
    boundary.note_cmd_edge();
    CHECK(boundary.consume() == 2);
    CHECK_FALSE(boundary.consume());
}

#include <algorithm>
#include <vector>

namespace {
struct CaptureUart {
    std::vector<std::uint8_t> software, driver;
    std::uint32_t errors = 0;
    bool available() {
        software.insert(software.end(), driver.begin(), driver.end());
        driver.clear();
        return !software.empty();
    }
    std::size_t read(std::uint8_t* out, std::size_t size) {
        auto n = std::min(size, software.size());
        std::copy_n(software.begin(), n, out);
        software.erase(software.begin(), software.begin() + n);
        return n;
    }
    void discardInput() { software.clear(); driver.clear(); }
    std::uint32_t rx_errors() const { return errors; }
};
struct CaptureRouter {
    std::vector<std::uint8_t> command, data;
    std::size_t pending = 0;
    std::size_t write_size = 2;
    void reset_command_collection() { command.clear(); pending = 0; }
    std::size_t pending_write_bytes() const { return pending; }
    void consume(std::uint8_t byte) {
        if (pending) { data.push_back(byte); if (data.size() == write_size + 1) pending = 0; }
        else { command.push_back(byte); if (command.size() == 5 && command[0] == 0x31) pending = write_size; }
    }
};
}

TEST_CASE("Atari SIO capture gates commands on release and separates write data")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = true;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge();
    uart.driver = {0x31, 1, 2};
    poll();
    CHECK(router.command.empty());
    uart.software = {3};
    uart.driver = {4};
    poll();
    CHECK(router.command.empty());
    low = false;
    boundary.note_cmd_edge();
    poll();
    CHECK((router.command == std::vector<std::uint8_t>{0x31, 1, 2, 3, 4}));
    CHECK(router.pending == 2);
    uart.driver = {10, 11, 12};
    poll();
    CHECK((router.data == std::vector<std::uint8_t>{10, 11, 12}));
    CHECK(router.pending == 0);
}

TEST_CASE("Atari SIO capture drops observed ambiguous edges and recovers")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    uart.software = {0x31}; uart.driver = {1, 2, 3, 4};
    boundary.note_cmd_edge(); boundary.note_cmd_edge();
    poll();
    CHECK(router.command.empty());
    CHECK(uart.software.empty()); CHECK(uart.driver.empty());
    poll();
    low = true; boundary.note_cmd_edge();
    uart.driver = {0x31, 1, 2, 3, 4}; poll();
    CHECK(router.command.empty());
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture aborts overflow and partial frames and discards stray input")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = false;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    uart.driver = {0x31, 1, 2, 3, 4}; poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    ++uart.errors; poll();
    low = false; boundary.note_cmd_edge(); poll(); poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    time += 50001; poll(); poll();
    CHECK(router.command.empty());
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture rejects excess command bytes and aborts a write on assertion")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart;
    CaptureRouter router;
    bool low = true;
    std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4, 5}; poll();
    CHECK(router.command.empty());
    low = false; boundary.note_cmd_edge(); poll(); poll();
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.pending == 2);
    low = true; boundary.note_cmd_edge(); uart.driver = {0x32, 1, 2, 3, 4}; poll();
    CHECK(router.pending == 0);
    CHECK(router.data.empty());
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.size() == 5);
    CHECK(router.command[0] == 0x32);
}

TEST_CASE("Atari SIO capture rejects an error seen before assertion poll")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = false; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    poll();
    ++uart.errors;
    low = true; boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll(); poll();
    CHECK(router.command.empty());
}

TEST_CASE("Atari SIO capture waits for final buffered byte after release")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    CHECK(router.command.empty());
    CHECK(capture.needs_immediate_poll());
    uart.driver = {4}; poll();
    CHECK(router.command.size() == 5);
}

TEST_CASE("Atari SIO capture extends write deadline on byte progress")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    time += 150000; uart.driver = {1}; poll();
    time += 150000; uart.driver = {2, 3}; poll();
    CHECK(router.data.size() == 3);
    CHECK(router.pending == 0);
}

TEST_CASE("Atari SIO capture accepts a 512-byte write longer than its idle timeout")
{
    fujinet::io::AtariSioCapture capture;
    fujinet::io::AtariSioTransactionBoundary boundary;
    CaptureUart uart; CaptureRouter router; router.write_size = 512;
    bool low = true; std::int64_t time = 1;
    auto poll = [&] { capture.poll(boundary, router, uart, [&] { return low; }, [&] { return time; }); };
    boundary.note_cmd_edge(); uart.driver = {0x31, 1, 2, 3, 4}; poll();
    low = false; boundary.note_cmd_edge(); poll();
    for (int i = 0; i < 32; ++i) {
        time += 8333; uart.driver.assign(16, 0x55); poll();
    }
    time += 521; uart.driver = {0}; poll();
    CHECK(router.data.size() == 513);
    CHECK(router.pending == 0);
}
