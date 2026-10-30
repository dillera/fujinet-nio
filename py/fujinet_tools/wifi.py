from __future__ import annotations

from typing import Optional
import getpass

from .common import open_serial, status_ok
from .fujibus import FujiBusSession

# -----------------------
# Wi-Fi service commands (docs/wifi_service_protocol.md)
# -----------------------
WIFI_SERVICE_ID = 0xF3
WIFI_CMD_GET_ADAPTER_INFO = 0x05
WIFI_CMD_TRY_JOIN = 0x06
WIFI_CMD_GET_JOIN_STATUS = 0x07
WIFI_VERSION = 1  # GET_ADAPTER_INFO: the highest version this client understands


def build_get_adapter_info_req() -> bytes:
    return bytes([WIFI_VERSION])


def parse_get_adapter_info_resp(payload: bytes) -> Optional[str]:
    """version, MAC-present (u8), six MAC bytes. Returns "aa:bb:cc:dd:ee:ff",
    or None when the adapter has no MAC to report. The reply's version is the
    lower of ours and the firmware's (protocol_reference.md, "Extending
    commands")."""
    if len(payload) < 8 or not 1 <= payload[0] <= WIFI_VERSION:
        raise ValueError("bad GET_ADAPTER_INFO response")
    if not payload[1]:
        return None
    return ":".join(f"{b:02x}" for b in payload[2:8])


def build_try_join_req(ssid: str, password: str) -> bytes:
    name, secret = ssid.encode("utf-8"), password.encode("utf-8")
    if not 1 <= len(name) <= 32 or len(secret) > 64:
        raise ValueError("SSID must be 1..32 bytes and password at most 64 bytes")
    return bytes([WIFI_VERSION, len(name)]) + name + bytes([len(secret)]) + secret


def build_join_status_req() -> bytes:
    return bytes([WIFI_VERSION])


def parse_join_status_resp(payload: bytes) -> int:
    if len(payload) < 2 or payload[0] != WIFI_VERSION or payload[1] > 3:
        raise ValueError("bad Wi-Fi join status response")
    return payload[1]


def cmd_wifi_join(args) -> int:
    password = "" if args.open else getpass.getpass("Wi-Fi password: ")
    try:
        request = build_try_join_req(args.ssid, password)
    except ValueError as exc:
        print(exc)
        return 1
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(device=WIFI_SERVICE_ID, command=WIFI_CMD_TRY_JOIN,
            payload=request, expect_device=WIFI_SERVICE_ID, expect_command=WIFI_CMD_TRY_JOIN,
            timeout=args.timeout, cmd_txt="WIFI_TRY_JOIN")
        if pkt is None or not status_ok(pkt):
            print("Wi-Fi trial was refused or unsupported")
            return 1
        print("Wi-Fi connection attempt started; use wifi join-status to check it")
    return 0


def cmd_wifi_join_status(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(device=WIFI_SERVICE_ID, command=WIFI_CMD_GET_JOIN_STATUS,
            payload=build_join_status_req(), expect_device=WIFI_SERVICE_ID,
            expect_command=WIFI_CMD_GET_JOIN_STATUS, timeout=args.timeout, cmd_txt="WIFI_GET_JOIN_STATUS")
        if pkt is None or not status_ok(pkt):
            print("Wi-Fi trial status unavailable")
            return 1
        try:
            state = parse_join_status_resp(pkt.payload)
        except ValueError as exc:
            print(exc)
            return 1
        print(("idle", "connecting", "connected", "failed; restoring previous network")[state])
        return 0 if state != 3 else 1


def cmd_wifi_adapter(args) -> int:
    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)
        pkt = bus.send_command_expect(
            device=WIFI_SERVICE_ID,
            command=WIFI_CMD_GET_ADAPTER_INFO,
            payload=build_get_adapter_info_req(),
            expect_device=WIFI_SERVICE_ID,
            expect_command=WIFI_CMD_GET_ADAPTER_INFO,
            timeout=args.timeout,
            cmd_txt="WIFI_GET_ADAPTER_INFO",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'} (older firmware answers Unsupported)")
            return 1
        try:
            mac = parse_get_adapter_info_resp(pkt.payload)
        except ValueError as e:
            print(f"Bad response: {e}")
            return 1
        print(f"mac: {mac or 'unavailable'}")
    return 0


def register_subcommands(subparsers) -> None:
    """Register Wi-Fi service commands under `wifi`."""
    pw = subparsers.add_parser("wifi", help="Wi-Fi service commands")
    wsub = pw.add_subparsers(dest="wifi_cmd", required=True)
    pwa = wsub.add_parser(
        "adapter",
        help="Show the Wi-Fi adapter's station MAC address",
        description="Ask the Wi-Fi service GET_ADAPTER_INFO for the station MAC address.",
    )
    pwa.set_defaults(fn=cmd_wifi_adapter)

    pwj = wsub.add_parser("join", help="Try a network and keep the previous one on failure")
    pwj.add_argument("ssid", help="Network name")
    pwj.add_argument("--open", action="store_true", help="Join an open network without a password")
    pwj.set_defaults(fn=cmd_wifi_join)
    pws = wsub.add_parser("join-status", help="Show the latest Wi-Fi trial result")
    pws.set_defaults(fn=cmd_wifi_join_status)
