// Palm HotSync: framing checked against bytes captured from a Palm OS 3.3
// device and palm-sync's NetSync recording; the sync flow checked against a
// fake device.

#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/hotsync/dlp.h"
#include "fujinet/hotsync/dlp_client.h"
#include "fujinet/hotsync/fs_storage.h"
#include "fujinet/hotsync/hotsync_init.h"
#include "fujinet/hotsync/cradle_channel.h"
#include "fujinet/hotsync/hotsync_service.h"
#include "fujinet/hotsync/netsync_transport.h"
#include "fujinet/hotsync/padp_transport.h"
#include "fujinet/hotsync/palm_database.h"
#include "fujinet/hotsync/session.h"
#include "fujinet/hotsync/slp.h"
#include "fujinet/io/core/baud_control.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fujinet::hotsync;

namespace {

Bytes hex(const std::string& s)
{
    Bytes out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Plays back device output in order and captures everything written.
class ScriptedLink : public Link {
public:
    std::deque<Bytes> reads;
    Bytes written;
    std::uint32_t baud{9600};

    int read(std::uint8_t* buf, std::size_t len, std::uint32_t) override
    {
        if (reads.empty()) {
            return -1;
        }
        Bytes& chunk = reads.front();
        const std::size_t n = std::min(len, chunk.size());
        std::memcpy(buf, chunk.data(), n);
        chunk.erase(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        if (chunk.empty()) {
            reads.pop_front();
        }
        return static_cast<int>(n);
    }
    int write(const std::uint8_t* buf, std::size_t len) override
    {
        written.insert(written.end(), buf, buf + len);
        return static_cast<int>(len);
    }
    void set_baud_rate(std::uint32_t b) override { baud = b; }
};

// Captured at the start of a sync: a stale ABORT, loopback probes, then WAKEUP.
const char* const kPalmWakeup =
    "beefed0303020004ffa508000000f593beefed03030300000baef9abbeefed03030300000caf701d"
    "beefed030302000effaf01c0000a0100010200000001c20086b1";
const char* const kPalmAckOfInit = "beefed0303020004ffa502c0000a1a85";
const char* const kHostAckOfWakeup = "beefed0303020004ffa502c0000a1a85";
const char* const kHostCmpInit = "beefed030302000effaf01c0000a0290010100000001c20003b0";

} // namespace

TEST_CASE("HotSync SLP: frame encoding matches a captured PADP ACK")
{
    SlpDatagram ack;
    ack.xid = 0xFF;
    ack.payload = hex("02c0000a");
    CHECK(slp_encode(ack) == hex(kHostAckOfWakeup));
}

TEST_CASE("HotSync SLP: the reader skips noise and rejects a corrupt frame")
{
    ScriptedLink link;
    Bytes corrupt = hex(kPalmAckOfInit);
    corrupt[12] ^= 0xFF;
    Bytes stream = hex("00beef11");
    stream.insert(stream.end(), corrupt.begin(), corrupt.end());
    const Bytes good = hex(kPalmAckOfInit);
    stream.insert(stream.end(), good.begin(), good.end());
    link.reads.push_back(stream);

    SlpReader reader(link);
    SlpDatagram datagram;
    REQUIRE(reader.read(datagram, 10));
    CHECK(datagram.xid == 0xFF);
    CHECK(datagram.payload == hex("02c0000a"));
    CHECK_FALSE(reader.read(datagram, 10));
    CHECK(reader.link_closed());
}

TEST_CASE("HotSync CMP: the handshake reproduces the captured exchange and raises the baud rate")
{
    ScriptedLink link;
    link.reads.push_back(hex(kPalmWakeup));
    link.reads.push_back(hex(kPalmAckOfInit));

    PadpTransport padp(link);
    REQUIRE(padp.accept());

    Bytes expected = hex(kHostAckOfWakeup);
    const Bytes init = hex(kHostCmpInit);
    expected.insert(expected.end(), init.begin(), init.end());
    CHECK(link.written == expected);
    CHECK(padp.baud_rate() == 115200);
    CHECK(link.baud == 115200);
}

TEST_CASE("HotSync CMP: waiting for WAKEUP stops before INIT, so a cradle can lift its listening deadline")
{
    ScriptedLink link;
    link.reads.push_back(hex(kPalmWakeup));
    link.reads.push_back(hex(kPalmAckOfInit));

    PadpTransport padp(link);
    REQUIRE(padp.wait_for_wakeup());
    CHECK(link.written == hex(kHostAckOfWakeup));
    CHECK(link.reads.size() == 1);

    REQUIRE(padp.answer_wakeup());
    CHECK(link.reads.empty());
    CHECK(link.baud == 115200);
}

TEST_CASE("HotSync CMP: a retransmitted WAKEUP is not taken as the ACK of our INIT")
{
    // WAKEUP, the same WAKEUP again (our ACK was "lost"), then the real ACK.
    const Bytes wakeup = hex("beefed030302000effaf01c0000a0100010200000001c20086b1");
    ScriptedLink link;
    link.reads.push_back(wakeup);
    link.reads.push_back(wakeup);
    link.reads.push_back(hex(kPalmAckOfInit));

    PadpTransport padp(link);
    REQUIRE(padp.accept());
    // Had the duplicate been read as an ACK, the real ACK would still be unread.
    std::uint8_t leftover;
    CHECK(link.read(&leftover, 1, 0) == -1);
}

TEST_CASE("HotSync PADP: a multi-fragment message is reassembled and each fragment ACKed")
{
    ScriptedLink link;
    auto fragment = [](std::uint8_t flags, std::uint16_t sizeOrOffset, const Bytes& data) {
        SlpDatagram d;
        d.xid = 7;
        d.payload = {0x01, flags, static_cast<std::uint8_t>(sizeOrOffset >> 8),
                     static_cast<std::uint8_t>(sizeOrOffset)};
        d.payload.insert(d.payload.end(), data.begin(), data.end());
        return slp_encode(d);
    };
    link.reads.push_back(fragment(0x80, 5, hex("0102")));
    link.reads.push_back(fragment(0x40, 2, hex("030405")));

    PadpTransport padp(link);
    Bytes message;
    REQUIRE(padp.receive(message, 10));
    CHECK(message == hex("0102030405"));

    SlpDatagram ackFirst;
    SlpDatagram ackLast;
    ackFirst.xid = ackLast.xid = 7;
    ackFirst.payload = hex("02c00005");
    ackLast.payload = hex("02c00002");
    Bytes expected = slp_encode(ackFirst);
    const Bytes second = slp_encode(ackLast);
    expected.insert(expected.end(), second.begin(), second.end());
    CHECK(link.written == expected);
}

TEST_CASE("HotSync NetSync: handshake and first request match palm-sync's recorded session")
{
    ScriptedLink link;
    link.reads.push_back(hex("01ff0000001690010000000000000020000000080200000000000000"));
    link.reads.push_back(hex("0101000000329201000000000000002000000024ffffffff3c003c000000004000000002"
                             "c0a80205e06b0000000000000000000000000000"));
    link.reads.push_back(hex("0102000000089300000000000000"));

    NetSyncTransport netsync(link);
    REQUIRE(netsync.accept());
    DlpRequest request(DlpFunc::ReadSysInfo);
    request.arg(DlpArgWriter().u16(1).u16(4));
    REQUIRE(netsync.send(request.encode()));

    CHECK(link.written ==
          hex("0101000000321201000000000000002000000024ffffffff3c003c000000000000000000c0a801210427"
              "0000000000000000000000000000"
              "01020000002e1301000000000000002000000020ffffffff003c003c0000000000000001000000000000"
              "00000000000000000000"
              "0103000000081201200400010004"));
}

TEST_CASE("HotSync NetSync: an oversized length is refused, not allocated")
{
    ScriptedLink link;
    link.reads.push_back(hex("0101ffffffff"));
    NetSyncTransport netsync(link);
    Bytes message;
    CHECK_FALSE(netsync.receive(message, 10));
    CHECK(message.empty());
}

TEST_CASE("HotSync DLP: request encoding matches the captured ReadSysInfo")
{
    DlpRequest request(DlpFunc::ReadSysInfo);
    request.arg(DlpArgWriter().u16(1).u16(4));
    CHECK(request.encode() == hex("1201200400010004"));
}

TEST_CASE("HotSync DLP: response decoding handles small and short arguments")
{
    // funcId|0x80, argc 2, no error; a small arg 0x20 then a short arg 0x21.
    const Bytes reply = hex("92020000" "2004" "03303000" "a1000003" "aabbcc");

    DlpResponse response;
    REQUIRE(response.decode(DlpFunc::ReadSysInfo, reply));
    CHECK(response.ok());
    CHECK(response.arg(0) == hex("03303000"));
    CHECK(response.arg(1) == hex("aabbcc"));
    CHECK_FALSE(response.decode(DlpFunc::ReadUserInfo, reply));
}

TEST_CASE("HotSync DLP: a long argument claiming more bytes than arrived fails cleanly")
{
    const Bytes reply = hex("92010000" "6000ffffffff" "aabb");
    DlpResponse response;
    CHECK_FALSE(response.decode(DlpFunc::ReadSysInfo, reply));
}

TEST_CASE("HotSync PDB: Palm timestamps convert both ways")
{
    DlpDateTime t;
    t.year = 2026;
    t.month = 9;
    t.day = 30;
    t.hour = 17;
    t.minute = 5;
    t.second = 9;
    const std::uint32_t seconds = palm_seconds_from_date(t);
    CHECK(seconds == 3873632709u);
    const DlpDateTime back = palm_date_from_seconds(seconds);
    CHECK(back.year == 2026);
    CHECK(back.month == 9);
    CHECK(back.day == 30);
    CHECK(back.hour == 17);
    CHECK(back.minute == 5);
    CHECK(back.second == 9);
    CHECK(palm_date_from_seconds(0).year == 1904);
}

namespace {

PalmDatabase sample_record_db()
{
    PalmDatabase db;
    db.info.name = "FujiTest";
    std::memcpy(db.info.type, "DATA", 4);
    std::memcpy(db.info.creator, "FjNt", 4);
    db.info.flags = kDbBackup;
    db.appInfo = hex("a1a2a3");
    DlpRecord live;
    live.id = 0x123456;
    live.category = 3;
    live.data = hex("10111213");
    DlpRecord archived;
    archived.id = 0x000102;
    archived.attributes = kRecordArchived;
    archived.data = hex("20");
    db.records = {live, archived};
    return db;
}

PalmDatabase sample_app(std::size_t codeBytes = 3000)
{
    PalmDatabase app;
    app.info.name = "Hello";
    std::memcpy(app.info.type, "appl", 4);
    std::memcpy(app.info.creator, "HeLo", 4);
    app.info.flags = kDbResource;
    DlpResource code;
    std::memcpy(code.type, "code", 4);
    code.id = 1;
    code.data = Bytes(codeBytes, 0x4E);
    app.resources = {code};
    return app;
}

} // namespace

TEST_CASE("HotSync PDB: files round-trip, keeping categories and the archive bit")
{
    const PalmDatabase db = sample_record_db();
    const Bytes file = db.serialize();

    PalmDatabase parsed;
    REQUIRE(parsed.parse(file));
    CHECK(parsed.info.name == "FujiTest");
    CHECK(std::string(parsed.info.creator) == "FjNt");
    CHECK(parsed.appInfo == hex("a1a2a3"));
    REQUIRE(parsed.records.size() == 2);
    CHECK(parsed.records[0].id == 0x123456);
    CHECK(parsed.records[0].category == 3);
    CHECK(parsed.records[0].data == hex("10111213"));
    CHECK((parsed.records[1].attributes & kRecordArchived) != 0);
    CHECK(parsed.records[1].data == hex("20"));
    CHECK(parsed.serialize() == file);
}

TEST_CASE("HotSync PDB: PRC files round-trip and truncated files are refused")
{
    const Bytes file = sample_app().serialize();
    PalmDatabase parsed;
    REQUIRE(parsed.parse(file));
    CHECK(parsed.is_resource_db());
    REQUIRE(parsed.resources.size() == 1);
    CHECK(std::string(parsed.resources[0].type) == "code");
    CHECK(parsed.resources[0].data.size() == 3000);
    CHECK(parsed.serialize() == file);

    CHECK_FALSE(PalmDatabase().parse(Bytes(file.begin(), file.begin() + 80)));
}

TEST_CASE("HotSync: backup file names are safe on FAT")
{
    DlpDbInfo info;
    info.name = "Graffiti/Short:Cuts";
    CHECK(backup_file_name(info) == "Graffiti_Short_Cuts.pdb");
    info.flags = kDbResource;
    CHECK(backup_file_name(info) == "Graffiti_Short_Cuts.prc");
}

namespace {

// Answers DLP requests like a device holding the databases in `databases`.
class FakePalm : public DlpTransport {
public:
    std::vector<DlpFunc> calls;
    std::map<std::string, PalmDatabase> databases;
    std::string userName;
    std::uint32_t userId{0};
    std::uint8_t writtenModFlags{0};
    std::vector<std::string> log;

    bool accept() override { return true; }

    bool send(const Bytes& message) override
    {
        DlpArgReader header(message);
        _func = static_cast<DlpFunc>(header.u8());
        calls.push_back(_func);
        const std::size_t argc = header.u8();
        _arg.clear();
        _argId = 0;
        if (argc > 0) {
            const std::uint8_t id = header.u8();
            _argId = id & 0x3F;
            std::size_t len = 0;
            if (id & 0x80) {
                header.skip(1);
                len = header.u16();
            } else {
                len = header.u8();
            }
            _arg = header.bytes(len);
        }
        return true;
    }

    bool receive(Bytes& message, std::uint32_t) override
    {
        DlpArgWriter reply;
        const std::uint16_t error = respond(reply);
        message = {static_cast<std::uint8_t>(static_cast<std::uint8_t>(_func) | 0x80),
                   static_cast<std::uint8_t>(reply.data().empty() ? 0 : 1),
                   static_cast<std::uint8_t>(error >> 8), static_cast<std::uint8_t>(error)};
        if (!reply.data().empty()) {
            DlpRequest wrapped(_func);
            wrapped.arg(reply);
            const Bytes encoded = wrapped.encode();
            message.insert(message.end(), encoded.begin() + 2, encoded.end());
        }
        return true;
    }

private:
    static void put_date(DlpArgWriter& w) { w.u16(2026).u8(10).u8(18).u8(12).u8(0).u8(0).u8(0); }

    PalmDatabase* by_handle(std::uint8_t handle)
    {
        return handle >= 1 && handle <= _open.size() ? _open[handle - 1] : nullptr;
    }

    std::uint8_t open(PalmDatabase& db)
    {
        _open.push_back(&db);
        return static_cast<std::uint8_t>(_open.size());
    }

    std::uint16_t respond(DlpArgWriter& reply)
    {
        DlpArgReader in(_arg);
        switch (_func) {
        case DlpFunc::ReadSysInfo:
            reply.u32(0x03303000).u32(0).u8(0).u8(0);
            return 0;
        case DlpFunc::ReadUserInfo:
            reply.u32(userId).u32(0).u32(0);
            for (int i = 0; i < 16; ++i) {
                reply.u8(0);
            }
            reply.u8(static_cast<std::uint8_t>(userName.empty() ? 0 : userName.size() + 1)).u8(0);
            if (!userName.empty()) {
                reply.cstring(userName);
            }
            return 0;
        case DlpFunc::WriteUserInfo:
            userId = in.u32();
            in.skip(8 + 8);
            writtenModFlags = in.u8();
            in.skip(1);
            userName = in.cstring();
            return 0;
        case DlpFunc::DeleteDB:
            in.skip(2);
            return databases.erase(in.cstring()) ? 0 : 5;
        case DlpFunc::CreateDB: {
            PalmDatabase db;
            const Bytes creator = in.bytes(4);
            const Bytes type = in.bytes(4);
            std::memcpy(db.info.creator, creator.data(), 4);
            std::memcpy(db.info.type, type.data(), 4);
            in.skip(2);
            db.info.flags = in.u16();
            db.info.version = in.u16();
            db.info.name = in.cstring();
            reply.u8(open(databases[db.info.name] = db));
            return 0;
        }
        case DlpFunc::OpenDB: {
            in.skip(2);
            auto found = databases.find(in.cstring());
            if (found == databases.end()) {
                return 5;
            }
            reply.u8(open(found->second));
            return 0;
        }
        case DlpFunc::WriteResource: {
            PalmDatabase* db = by_handle(in.u8());
            in.skip(1);
            DlpResource r;
            const Bytes type = in.bytes(4);
            std::memcpy(r.type, type.data(), 4);
            r.id = in.u16();
            r.data = in.bytes(in.u16());
            db->resources.push_back(r);
            return 0;
        }
        case DlpFunc::ReadDBList: {
            in.skip(2);
            const std::uint16_t start = in.u16();
            if (start >= databases.size()) {
                return 5;
            }
            reply.u16(static_cast<std::uint16_t>(databases.size() - 1)).u8(0)
                .u8(static_cast<std::uint8_t>(databases.size() - start));
            std::uint16_t index = 0;
            for (const auto& [name, db] : databases) {
                if (index++ < start) {
                    continue;
                }
                DlpArgWriter entry;
                entry.u8(0).u16(db.info.flags).type_id(db.info.type).type_id(db.info.creator).u16(0).u32(0);
                put_date(entry);
                put_date(entry);
                put_date(entry);
                entry.u16(static_cast<std::uint16_t>(index - 1)).cstring(name);
                reply.u8(static_cast<std::uint8_t>(entry.data().size() + 1)).bytes(entry.data());
            }
            return 0;
        }
        case DlpFunc::ReadOpenDBInfo: {
            const PalmDatabase* db = by_handle(in.u8());
            reply.u16(static_cast<std::uint16_t>(db->is_resource_db() ? db->resources.size() : db->records.size()));
            return 0;
        }
        case DlpFunc::ReadAppBlock: {
            const PalmDatabase* db = by_handle(in.u8());
            if (db->appInfo.empty()) {
                return 5;
            }
            reply.u16(static_cast<std::uint16_t>(db->appInfo.size())).bytes(db->appInfo);
            return 0;
        }
        case DlpFunc::ReadSortBlock:
            return 5;
        case DlpFunc::ReadRecord: {
            const PalmDatabase* db = by_handle(in.u8());
            in.skip(1);
            const std::uint16_t index = in.u16();
            if (_argId != 0x21 || index >= db->records.size()) {
                return 5;
            }
            const DlpRecord& r = db->records[index];
            reply.u32(r.id).u16(index).u16(static_cast<std::uint16_t>(r.data.size())).u8(r.attributes)
                .u8(r.category).bytes(r.data);
            return 0;
        }
        case DlpFunc::ReadResource: {
            const PalmDatabase* db = by_handle(in.u8());
            in.skip(1);
            const std::uint16_t index = in.u16();
            if (index >= db->resources.size()) {
                return 5;
            }
            const DlpResource& r = db->resources[index];
            reply.type_id(r.type).u16(r.id).u16(index).u16(static_cast<std::uint16_t>(r.data.size())).bytes(r.data);
            return 0;
        }
        case DlpFunc::AddSyncLogEntry:
            log.push_back(in.cstring());
            return 0;
        default:
            return 0;
        }
    }

    DlpFunc _func{DlpFunc::EndOfSync};
    std::uint8_t _argId{0};
    Bytes _arg;
    std::vector<PalmDatabase*> _open;
};

class MemoryStorage : public Storage {
public:
    std::map<std::string, Bytes> install;
    std::vector<std::string> installed;
    std::map<std::string, Bytes> backups; // "<user>/<file>"

    std::vector<std::string> pending_installs() override
    {
        std::vector<std::string> names;
        for (const auto& entry : install) {
            names.push_back(entry.first);
        }
        return names;
    }
    bool read_install(const std::string& name, Bytes& out, std::size_t maxBytes) override
    {
        out = install[name];
        return out.size() <= maxBytes;
    }
    bool mark_installed(const std::string& name) override
    {
        installed.push_back(name);
        return true;
    }
    bool write_backup(const std::string& user, const std::string& name, const Bytes& data) override
    {
        backups[user + "/" + name] = data;
        return true;
    }
};

} // namespace

TEST_CASE("HotSync session: installs queued apps and names a new device")
{
    MemoryStorage storage;
    storage.install["hello.prc"] = sample_app().serialize();
    storage.install["readme.txt"] = hex("00");

    FakePalm palm;
    SessionOptions options;
    options.defaultUserName = "FujiNet";
    options.newUserId = 42;
    options.backup = BackupPolicy::None;
    options.now.year = 2026;
    const SessionReport report = Session(palm, storage, options).run();

    CHECK(report.error == DlpError::None);
    CHECK(report.installed == 1);
    CHECK(report.userName == "FujiNet");
    REQUIRE(palm.databases.count("Hello") == 1);
    REQUIRE(palm.databases["Hello"].resources.size() == 1);
    CHECK(palm.databases["Hello"].resources[0].data.size() == 3000);
    CHECK(storage.installed == std::vector<std::string>{"hello.prc"});
    CHECK(palm.userName == "FujiNet");
    CHECK(palm.userId == 42);
    CHECK((palm.writtenModFlags & (kModUserId | kModUserName)) == (kModUserId | kModUserName));
    CHECK((palm.writtenModFlags & kModLastSyncDate) != 0);
    CHECK(palm.calls.back() == DlpFunc::EndOfSync);
    CHECK(std::find(palm.log.begin(), palm.log.end(), "Installed hello.prc\n") != palm.log.end());
}

TEST_CASE("HotSync session: backs up flagged databases under the user's name")
{
    FakePalm palm;
    palm.userName = "Ada/Palm";
    palm.userId = 7;
    palm.databases["FujiTest"] = sample_record_db();
    PalmDatabase unflagged = sample_record_db();
    unflagged.info.name = "Scratch";
    unflagged.info.flags = 0;
    palm.databases["Scratch"] = unflagged;
    PalmDatabase prefs = sample_record_db();
    prefs.info.name = "Unsaved Preferences";
    palm.databases["Unsaved Preferences"] = prefs;

    MemoryStorage storage;
    SessionOptions options;
    options.backup = BackupPolicy::Flagged;
    const SessionReport report = Session(palm, storage, options).run();

    CHECK(report.error == DlpError::None);
    CHECK(report.backedUp == 1);
    CHECK(report.userName == "Ada/Palm");
    REQUIRE(storage.backups.size() == 1);
    REQUIRE(storage.backups.count("Ada_Palm/FujiTest.pdb") == 1);
    PalmDatabase saved;
    REQUIRE(saved.parse(storage.backups["Ada_Palm/FujiTest.pdb"]));
    CHECK(saved.appInfo == hex("a1a2a3"));
    REQUIRE(saved.records.size() == 2);
    CHECK(saved.records[0].category == 3);
    // An existing user keeps its name and ID.
    CHECK((palm.writtenModFlags & (kModUserId | kModUserName)) == 0);
}

TEST_CASE("HotSync session: a backup of every database includes unflagged ones")
{
    FakePalm palm;
    palm.userName = "Ada";
    palm.userId = 7;
    palm.databases["FujiTest"] = sample_record_db();
    palm.databases["Hello"] = sample_app();

    MemoryStorage storage;
    SessionOptions options;
    options.backup = BackupPolicy::All;
    const SessionReport report = Session(palm, storage, options).run();

    CHECK(report.backedUp == 2);
    CHECK(storage.backups.count("Ada/Hello.prc") == 1);
}

TEST_CASE("HotSync session: databases over the size cap are skipped, not loaded")
{
    FakePalm palm;
    palm.userName = "Ada";
    palm.userId = 7;
    palm.databases["Big"] = sample_app(4000);
    palm.databases["Big"].info.name = "Big";

    MemoryStorage storage;
    storage.install["huge.prc"] = sample_app(4000).serialize();

    SessionOptions options;
    options.backup = BackupPolicy::All;
    options.maxDatabaseBytes = 2048;
    const SessionReport report = Session(palm, storage, options).run();

    CHECK(report.error == DlpError::None);
    CHECK(report.installFailures == 1);
    CHECK(report.backupFailures == 1);
    CHECK(storage.installed.empty());
    CHECK(storage.backups.empty());
}

TEST_CASE("HotSync storage: queue, install and back up in a filesystem folder")
{
    fujinet::tests::MemoryFileSystem fs("host");
    FsStorage storage(fs, "/hotsync/");
    REQUIRE(storage.create_install_folder());
    CHECK(fs.isDirectory("/hotsync/install"));

    const Bytes app = sample_app().serialize();
    fs.create_file("/hotsync/install/hello.prc", app);
    fs.create_file("/hotsync/install/notes.pdb", hex("00"));
    CHECK(storage.pending_installs() == std::vector<std::string>{"hello.prc", "notes.pdb"});

    Bytes read;
    REQUIRE(storage.read_install("hello.prc", read, 1024 * 1024));
    CHECK(read == app);
    CHECK_FALSE(storage.read_install("hello.prc", read, 100));

    REQUIRE(storage.mark_installed("hello.prc"));
    CHECK_FALSE(fs.exists("/hotsync/install/hello.prc"));
    CHECK(fs.exists("/hotsync/installed/hello.prc"));
    CHECK(storage.pending_installs() == std::vector<std::string>{"notes.pdb"});

    REQUIRE(storage.write_backup("Ada", "Memo.pdb", hex("0102")));
    fujinet::fs::FileInfo info;
    REQUIRE(fs.stat("/hotsync/backup/Ada/Memo.pdb", info));
    CHECK(info.sizeBytes == 2);
}

TEST_CASE("HotSync config: the hotsync block round-trips through YAML")
{
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("primary");
    fujinet::config::YamlFujiConfigStoreFs store(fs.get(), nullptr, "fujinet.yaml");

    fujinet::config::FujiConfig defaults = store.load();
    CHECK_FALSE(defaults.hotsync.enabled);
    CHECK(defaults.hotsync.port == 14238);
    CHECK_FALSE(defaults.hotsync.cradle);
    CHECK(defaults.hotsync.backup == "flagged");

    fujinet::config::FujiConfig cfg;
    cfg.hotsync.enabled = true;
    cfg.hotsync.port = 14000;
    cfg.hotsync.cradle = true;
    cfg.hotsync.root = "/palm";
    cfg.hotsync.userName = "Desk";
    cfg.hotsync.backup = "all";
    store.save(cfg);

    const fujinet::config::FujiConfig loaded = store.load();
    CHECK(loaded.hotsync.enabled);
    CHECK(loaded.hotsync.port == 14000);
    CHECK(loaded.hotsync.cradle);
    CHECK(loaded.hotsync.root == "/palm");
    CHECK(loaded.hotsync.userName == "Desk");
    CHECK(loaded.hotsync.backup == "all");
}

TEST_CASE("HotSync config: backup policy names parse both ways")
{
    BackupPolicy policy = BackupPolicy::None;
    REQUIRE(parse_backup_policy("all", policy));
    CHECK(policy == BackupPolicy::All);
    CHECK(std::string(backup_policy_name(policy)) == "all");
    CHECK_FALSE(parse_backup_policy("some", policy));
}

namespace {

class MemoryConfigStore final : public fujinet::config::FujiConfigStore {
public:
    fujinet::config::FujiConfig load() override { return saved; }
    void save(const fujinet::config::FujiConfig& cfg) override
    {
        saved = cfg;
        ++saves;
    }
    fujinet::config::FujiConfig saved;
    int saves{0};
};

fujinet::diag::DiagResult run_diag(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    fujinet::diag::DiagArgsView args;
    args.line = argv.empty() ? std::string_view{} : argv[0];
    args.argv = std::move(argv);
    return p.execute(args);
}

} // namespace

TEST_CASE("HotSync diagnostics: status says how to turn HotSync on when it is off")
{
    auto provider = fujinet::diag::create_hotsync_diagnostic_provider(nullptr, nullptr);
    const fujinet::diag::DiagResult r = run_diag(*provider, {"hotsync.status"});
    CHECK(r.status == fujinet::diag::DiagStatus::Ok);
    CHECK(r.text == "enabled: 0\r\n(hotsync.set enabled 1, hotsync.save, then restart)\r\n");
    CHECK(run_diag(*provider, {"hotsync.get"}).status == fujinet::diag::DiagStatus::NotReady);
    CHECK(run_diag(*provider, {"hotsync.save"}).status == fujinet::diag::DiagStatus::NotReady);
}

TEST_CASE("HotSync diagnostics: set changes the stored settings and save writes them")
{
    auto storeOwned = std::make_unique<MemoryConfigStore>();
    MemoryConfigStore& store = *storeOwned;
    fujinet::io::FujiDevice fuji(nullptr, std::move(storeOwned));
    auto provider = fujinet::diag::create_hotsync_diagnostic_provider(nullptr, &fuji);
    using fujinet::diag::DiagStatus;

    CHECK(run_diag(*provider, {"hotsync.get"}).text ==
          "enabled: 0\r\nport: 14238\r\ncradle: 0\r\nroot: /hotsync\r\nuser_name: FujiNet\r\nbackup: flagged\r\n");

    CHECK(run_diag(*provider, {"hotsync.set", "enabled", "1"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.set", "port", "14239"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.set", "cradle", "1"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.set", "root", "/palm"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.set", "user_name", "Andy", "D"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.set", "backup", "ALL"}).status == DiagStatus::Ok);
    CHECK(run_diag(*provider, {"hotsync.get"}).text ==
          "enabled: 1\r\nport: 14239\r\ncradle: 1\r\nroot: /palm\r\nuser_name: Andy D\r\nbackup: all\r\n");
    CHECK(store.saves == 0);

    CHECK(run_diag(*provider, {"hotsync.save"}).status == DiagStatus::Ok);
    CHECK(store.saves == 1);
    CHECK(store.saved.hotsync.enabled);
    CHECK(store.saved.hotsync.port == 14239);
    CHECK(store.saved.hotsync.cradle);
    CHECK(store.saved.hotsync.userName == "Andy D");
    CHECK(store.saved.hotsync.backup == "all");
}

TEST_CASE("HotSync diagnostics: set refuses bad values and leaves the setting alone")
{
    fujinet::io::FujiDevice fuji(nullptr, std::make_unique<MemoryConfigStore>());
    auto provider = fujinet::diag::create_hotsync_diagnostic_provider(nullptr, &fuji);
    using fujinet::diag::DiagStatus;

    CHECK(run_diag(*provider, {"hotsync.set", "enabled", "yes"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "cradle", "on"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "port", "-1"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "port", "65536"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "root", "hotsync"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "user_name", std::string(41, 'x')}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "backup", "some"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "colour", "red"}).status == DiagStatus::InvalidArgs);
    CHECK(run_diag(*provider, {"hotsync.set", "port"}).status == DiagStatus::InvalidArgs);
    CHECK(fuji.config().hotsync.port == 14238);
    CHECK(fuji.config().hotsync.root == "/hotsync");
    CHECK(fuji.config().hotsync.backup == "flagged");
}

namespace {

// Listens on fd 3 and hands out one connection, fd 4, whose peer has already
// hung up: a port probe.
struct ProbeSocketOps final : fujinet::net::ITcpSocketOps {
    bool accepted = false;
    std::uint64_t now = 0;

    int socket(int, int, int) override { return 3; }
    void close(int) override {}
    int connect(int, const struct sockaddr*, fujinet::net::SockLen) override { return -1; }
    int bind(int, const struct sockaddr*, fujinet::net::SockLen) override { return 0; }
    int listen(int, int) override { return 0; }
    int accept(int, struct sockaddr*, fujinet::net::SockLen*) override
    {
        if (accepted) return -1;
        accepted = true;
        return 4;
    }
    int set_nonblocking(int) override { return 0; }
    bool poll_connect_complete(int) override { return false; }
    fujinet::net::SSize send(int, const void*, std::size_t len) override
    {
        return static_cast<fujinet::net::SSize>(len);
    }
    fujinet::net::SSize recv(int, void*, std::size_t) override { return 0; }
    int shutdown_write(int) override { return 0; }
    int get_so_error(int) override { return 0; }
    int setsockopt(int, int, int, const void*, fujinet::net::SockLen) override { return 0; }
    void apply_stream_socket_options(int, bool, bool) override {}
    void apply_listen_socket_options(int) override {}
    int getaddrinfo(const char*, const char*, const void*, fujinet::net::AddrInfo** res) override
    {
        *res = reinterpret_cast<fujinet::net::AddrInfo*>(&now);
        return 0;
    }
    const void* tcp_stream_addrinfo_hints() const noexcept override { return nullptr; }
    const void* tcp_stream_passive_addrinfo_hints() const noexcept override { return nullptr; }
    void freeaddrinfo(fujinet::net::AddrInfo*) override {}
    fujinet::net::AddrInfo* addrinfo_next(fujinet::net::AddrInfo*) override { return nullptr; }
    int addrinfo_family(fujinet::net::AddrInfo*) override { return 0; }
    int addrinfo_socktype(fujinet::net::AddrInfo*) override { return 0; }
    int addrinfo_protocol(fujinet::net::AddrInfo*) override { return 0; }
    const struct sockaddr* addrinfo_addr(fujinet::net::AddrInfo*, fujinet::net::SockLen*) override { return nullptr; }
    std::uint64_t now_ms() override { return now; }
    int last_errno() override { return 0; }
    const char* err_string(int) override { return "probe socket"; }
    bool is_would_block(int) const noexcept override { return false; }
    bool is_in_progress(int) const noexcept override { return false; }
    bool is_peer_gone(int) const noexcept override { return true; }
    int err_timed_out() const noexcept override { return 110; }
    int err_conn_refused() const noexcept override { return 111; }
    int err_host_unreach() const noexcept override { return 113; }
};

} // namespace

TEST_CASE("HotSync service: a connection that never completes the handshake is rejected, not a failed sync")
{
    ProbeSocketOps ops;
    fujinet::tests::MemoryFileSystem fs("host");
    HotSyncService service(ops, fs, HotSyncSettings{}, nullptr);

    CHECK(service.poll());
    const HotSyncStatus s = service.status();
    CHECK(s.listening);
    CHECK(s.rejected == 1);
    CHECK(s.syncs == 0);
    CHECK(s.failures == 0);
    CHECK(s.lastResult.empty());
}

// ---------------------------------------------------------------------------
// Cradle: one serial line shared by FujiBus and HotSync.

namespace {

// A serial line: bytes the far end sent, bytes written to it, and its rate.
class FakeLine final : public fujinet::io::Channel, public fujinet::io::BaudControl {
public:
    Bytes rx;
    Bytes tx;
    std::uint32_t baud{115200};
    std::vector<std::uint32_t> rates; // every rate change, in order

    void receive(const Bytes& bytes) { rx.insert(rx.end(), bytes.begin(), bytes.end()); }

    bool available() override { return !rx.empty(); }
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        const std::size_t n = std::min(maxLen, rx.size());
        std::memcpy(buffer, rx.data(), n);
        rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    void write(const std::uint8_t* buffer, std::size_t len) override { tx.insert(tx.end(), buffer, buffer + len); }

    std::uint32_t baud_rate() const override { return baud; }
    bool set_baud_rate(std::uint32_t b) override
    {
        baud = b;
        rates.push_back(b);
        return true;
    }
};

struct CradleFixture {
    std::uint64_t now{1000};
    FakeLine* line{nullptr};
    std::unique_ptr<CradleChannel> cradle;

    CradleFixture()
    {
        auto owned = std::make_unique<FakeLine>();
        line = owned.get();
        CradleTiming timing;
        timing.settleMs = 0;
        cradle = std::make_unique<CradleChannel>(std::move(owned), *line, [this] { return now; }, timing);
    }

    // What FujiBus reads off the cradle now.
    Bytes bus_read()
    {
        Bytes out;
        std::uint8_t buf[64];
        while (cradle->available()) {
            const std::size_t n = cradle->read(buf, sizeof(buf));
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    }
};

// A FujiBus packet: device 0x70, command 0x01, length 6, checksum 0x77.
const Bytes kFujiBusFrame = {0xC0, 0x70, 0x01, 0x06, 0x00, 0x77, 0x00, 0xC0};

} // namespace

TEST_CASE("HotSync cradle: FujiBus traffic passes through and holds the line for a minute")
{
    CradleFixture f;
    f.line->receive(kFujiBusFrame);
    CHECK(f.bus_read() == kFujiBusFrame);
    const std::uint8_t reply[] = {0xC0, 0x01, 0xC0};
    f.cradle->write(reply, sizeof(reply));
    CHECK(f.line->tx == Bytes(reply, reply + sizeof(reply)));

    CHECK(f.cradle->host_active());
    CHECK_FALSE(f.cradle->claim(9600));
    f.now += 59999;
    CHECK_FALSE(f.cradle->claim(9600));
    f.now += 1;
    CHECK(f.cradle->claim(9600));
    CHECK(f.cradle->stats().refused == 2);
    CHECK(f.cradle->stats().frames == 1);
}

TEST_CASE("HotSync cradle: stray bytes away from any frame end the hold early")
{
    CradleFixture f;
    f.line->receive(kFujiBusFrame);
    f.bus_read();

    // Close behind a frame they are line noise, not a Palm.
    f.now += 500;
    f.line->receive({0x00, 0xF8});
    f.bus_read();
    CHECK(f.cradle->host_active());

    // A 9600-baud WAKEUP read at the bus rate, with no frame near it.
    f.now += 1500;
    f.line->receive({0x00, 0x80, 0xF8, 0x00});
    f.bus_read();
    CHECK_FALSE(f.cradle->host_active());
    CHECK(f.cradle->claim(9600));
    CHECK(f.cradle->stats().strayBytes == 6);
}

TEST_CASE("HotSync cradle: a Palm's WAKEUP holds SLIP END bytes but is not FujiBus")
{
    // Captured on the emulator rig: two WAKEUPs (their PADP flags byte is
    // 0xC0), then a Palm app's FujiBus request.
    CradleFixture f;
    const Bytes wakeup = hex("beefed030302000effaf01c0000a0100010200000001c20086b1");
    f.line->receive(wakeup);
    f.line->receive(wakeup);
    f.bus_read();
    CHECK(f.cradle->stats().frames == 0);
    CHECK_FALSE(f.cradle->host_active());

    f.line->receive(hex("c070e806005f00c0"));
    f.bus_read();
    CHECK(f.cradle->stats().frames == 1);
    CHECK(f.cradle->host_active());
}

TEST_CASE("HotSync cradle: escaped bytes are decoded before the length and checksum check")
{
    // Length 7, one payload byte 0xC0 sent as DB DC; checksum 0x39.
    CradleFixture f;
    f.line->receive({0xC0, 0x70, 0x01, 0x07, 0x00, 0x39, 0x00, 0xDB, 0xDC, 0xC0});
    f.bus_read();
    CHECK(f.cradle->stats().frames == 1);
    CHECK(f.cradle->stats().strayBytes == 0);
}

TEST_CASE("HotSync cradle: a request still waiting at claim time keeps the line and reaches FujiBus")
{
    CradleFixture f;
    f.line->receive(kFujiBusFrame);
    CHECK_FALSE(f.cradle->claim(9600));
    CHECK(f.line->rates.empty());
    CHECK(f.bus_read() == kFujiBusFrame);
}

TEST_CASE("HotSync cradle: a lent line is silent to FujiBus, and release restores the rate")
{
    CradleFixture f;
    f.line->receive({0x00, 0x00}); // stray bytes left at claim time are dropped
    REQUIRE(f.cradle->claim(9600));
    CHECK(f.cradle->lent());
    CHECK(f.line->baud == 9600);
    CHECK(f.line->rx.empty());

    // FujiBus neither reads what the Palm sends nor writes over it.
    f.line->receive({0xBE, 0xEF, 0xED});
    CHECK_FALSE(f.cradle->available());
    std::uint8_t buf[8];
    CHECK(f.cradle->read(buf, sizeof(buf)) == 0);
    const std::uint8_t reply[] = {0xC0, 0x01, 0xC0};
    f.cradle->write(reply, sizeof(reply));
    CHECK(f.line->tx.empty());
    CHECK(f.cradle->stats().droppedWrites == 1);

    // The lent link sees the Palm and writes to it.
    CHECK(f.cradle->link().read(buf, sizeof(buf), 0) == 3);
    CHECK(f.cradle->link().write(reply, sizeof(reply)) == 3);
    CHECK(f.line->tx.size() == 3);
    f.cradle->link().set_baud_rate(57600);
    CHECK(f.line->baud == 57600);

    // Bytes left at the Palm's rate are dropped; the bus rate comes back.
    f.line->receive({0x12, 0x34});
    f.cradle->release();
    CHECK_FALSE(f.cradle->lent());
    CHECK(f.line->baud == 115200);
    CHECK(f.line->rates == std::vector<std::uint32_t>{9600, 57600, 115200});
    CHECK(f.line->rx.empty());
    CHECK(f.cradle->link().read(buf, sizeof(buf), 0) == -1);

    f.line->receive(kFujiBusFrame);
    CHECK(f.bus_read() == kFujiBusFrame);
}

TEST_CASE("HotSync cradle: a WAKEUP inside a window is answered on the lent line at the Palm's rate")
{
    CradleFixture f;
    REQUIRE(f.cradle->claim(kCmpInitialBaud));
    f.line->receive(hex(kPalmWakeup));
    f.line->receive(hex(kPalmAckOfInit));

    PadpTransport padp(f.cradle->link());
    REQUIRE(padp.wait_for_wakeup(100));
    REQUIRE(padp.answer_wakeup());
    Bytes expected = hex(kHostAckOfWakeup);
    const Bytes init = hex(kHostCmpInit);
    expected.insert(expected.end(), init.begin(), init.end());
    CHECK(f.line->tx == expected);
    CHECK(f.line->baud == 115200);

    f.cradle->release();
    CHECK(f.line->rates == std::vector<std::uint32_t>{9600, 115200, 115200});
}

TEST_CASE("HotSync cradle: a window with no WAKEUP gives the line back")
{
    CradleFixture f;
    REQUIRE(f.cradle->claim(kCmpInitialBaud));
    PadpTransport padp(f.cradle->link());
    CHECK_FALSE(padp.wait_for_wakeup(20));
    f.cradle->release();
    CHECK(f.line->baud == 115200);
    CHECK(f.line->tx.empty());
}

TEST_CASE("HotSync cradle: a window closes on time while bytes keep arriving")
{
    // A Palm app retrying FujiBus requests more often than the window lasts,
    // or bus bytes at the wrong rate: never a WAKEUP, never a quiet line.
    class NoisyLink : public Link {
    public:
        int read(std::uint8_t* buf, std::size_t len, std::uint32_t) override
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const std::uint8_t request[] = {0xC0, 0x70, 0x01, 0x07, 0x00, 0x79, 0x00, 0x01, 0xC0};
            const std::size_t n = std::min(len, sizeof(request));
            std::memcpy(buf, request, n);
            return static_cast<int>(n);
        }
        int write(const std::uint8_t*, std::size_t len) override { return static_cast<int>(len); }
        void set_baud_rate(std::uint32_t) override {}
    } link;

    PadpTransport padp(link);
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(padp.wait_for_wakeup(50));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("HotSync service: cradle windows alternate with the bus and stop while an app is active")
{
    CradleFixture f;
    ProbeSocketOps ops;
    fujinet::tests::MemoryFileSystem fs("host");
    HotSyncSettings settings;
    settings.port = 0; // cradle only
    CradleWindow window;
    window.listenMs = 20;
    window.gapMs = 0;
    HotSyncService service(ops, fs, settings, nullptr, f.cradle.get(), window);

    CHECK(service.poll());
    HotSyncStatus s = service.status();
    CHECK(s.cradle == "idle");
    CHECK(s.cradleWindows == 1);
    CHECK_FALSE(s.listening);
    CHECK(s.syncs == 0);
    CHECK(f.line->rates == std::vector<std::uint32_t>{9600, 115200});

    // A Palm app talks FujiBus: no window opens.
    f.line->receive(kFujiBusFrame);
    f.bus_read();
    CHECK_FALSE(service.poll());
    s = service.status();
    CHECK(s.cradle == "app");
    CHECK(s.cradleWindows == 1);
    CHECK(f.line->rates.size() == 2);
}
