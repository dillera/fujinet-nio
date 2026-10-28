from __future__ import annotations

import unittest

from fujinet_tools import netproto as np
from fujinet_tools.analyze_capture import _command_name


class ReadyTests(unittest.TestCase):
    def test_command_and_request(self) -> None:
        self.assertEqual(np.CMD_READY, 0x08)
        self.assertEqual(np.build_ready_req(), b"\x01")
        self.assertEqual(_command_name(np.NETWORK_DEVICE_ID, np.CMD_READY), "Ready")

    def test_parse_ready_response(self) -> None:
        # version, flags, reserved, count=2, {handle 0x0100, read, 300}, {handle 0x0201, info, 0}
        payload = bytes([1, 0, 0, 0, 2]) + bytes([0x00, 0x01, 0x01, 0x2C, 0x01]) + bytes([0x01, 0x02, 0x02, 0x00, 0x00])
        entries = np.parse_ready_resp(payload)
        self.assertEqual([e.handle for e in entries], [0x0100, 0x0201])
        self.assertTrue(entries[0].readable)
        self.assertFalse(entries[0].info_ready)
        self.assertEqual(entries[0].available, 300)
        self.assertTrue(entries[1].info_ready)
        self.assertFalse(entries[1].readable)

    def test_empty_and_malformed(self) -> None:
        self.assertEqual(np.parse_ready_resp(bytes([1, 0, 0, 0, 0])), [])
        with self.assertRaises(ValueError):
            np.parse_ready_resp(bytes([1, 0, 0, 0, 0, 9]))   # trailing byte
        with self.assertRaises(ValueError):
            np.parse_ready_resp(bytes([2, 0, 0, 0, 0]))       # wrong version


class ReadAvailableTests(unittest.TestCase):
    def _read_payload(self, flags: int, reserved: int, data: bytes) -> bytes:
        return (
            bytes([1, flags])
            + reserved.to_bytes(2, "little")
            + (5).to_bytes(2, "little")          # handle
            + (0).to_bytes(4, "little")          # offset
            + len(data).to_bytes(2, "little")
            + data
        )

    def test_available_reported_when_flagged(self) -> None:
        r = np.parse_read_resp(self._read_payload(0x04 | np.READ_FLAG_AVAILABLE_VALID, 77, b"ab"))
        self.assertEqual(r.available, 77)
        self.assertTrue(r.more_available)
        self.assertEqual(r.data, b"ab")

    def test_available_unknown_from_older_firmware(self) -> None:
        r = np.parse_read_resp(self._read_payload(0x01, 0, b""))
        self.assertIsNone(r.available)
        self.assertTrue(r.eof)


if __name__ == "__main__":
    unittest.main()
