import unittest

from fujinet_tools import fuji, wifi
from fujinet_tools.analyze_capture import COMMAND_NAMES, DEVICE_NAMES


class FujiGetInfoTests(unittest.TestCase):
    def test_request_is_the_version(self):
        self.assertEqual(fuji.build_get_info_req(), b"\x01")

    def test_parses_version_and_profile(self):
        payload = b"\x01" + bytes([5]) + b"0.1.1" + bytes([7]) + b"Generic"
        info = fuji.parse_get_info_resp(payload)
        self.assertEqual((info.firmware_version, info.build_profile), ("0.1.1", "Generic"))

    def test_tolerates_trailing_bytes(self):
        payload = b"\x01\x01a\x01b" + b"\xAA\xBB"
        self.assertEqual(fuji.parse_get_info_resp(payload).build_profile, "b")

    def test_rejects_bad_responses(self):
        for payload in (b"", b"\x02\x00\x00", b"\x01\x05abc", b"\x01\x01a"):
            with self.subTest(payload=payload):
                with self.assertRaises(ValueError):
                    fuji.parse_get_info_resp(payload)


class WifiAdapterInfoTests(unittest.TestCase):
    def test_request_is_the_version(self):
        self.assertEqual(wifi.build_get_adapter_info_req(), b"\x01")

    def test_parses_mac(self):
        payload = bytes([1, 1, 0x24, 0x6F, 0x28, 0x01, 0x02, 0x03])
        self.assertEqual(wifi.parse_get_adapter_info_resp(payload), "24:6f:28:01:02:03")

    def test_mac_not_present(self):
        self.assertIsNone(wifi.parse_get_adapter_info_resp(bytes([1, 0]) + bytes(6)))

    def test_rejects_bad_responses(self):
        for payload in (b"", bytes([1, 1, 0, 0]), bytes([2, 1]) + bytes(6)):
            with self.subTest(payload=payload):
                with self.assertRaises(ValueError):
                    wifi.parse_get_adapter_info_resp(payload)


class WifiJoinTests(unittest.TestCase):
    def test_request_and_boundaries(self):
        self.assertEqual(wifi.build_try_join_req("Home", "secret"), b"\x01\x04Home\x06secret")
        self.assertEqual(wifi.build_try_join_req("open", ""), b"\x01\x04open\x00")
        for ssid, password in (("", ""), ("x" * 33, ""), ("x", "y" * 65)):
            with self.assertRaises(ValueError):
                wifi.build_try_join_req(ssid, password)
        self.assertEqual(wifi.build_join_status_req(), b"\x01")

    def test_status(self):
        for state in range(4):
            self.assertEqual(wifi.parse_join_status_resp(bytes([1, state, 99])), state)
        for bad in (b"", b"\x01", b"\x02\x01", b"\x01\x04"):
            with self.assertRaises(ValueError):
                wifi.parse_join_status_resp(bad)


class CaptureNamesTests(unittest.TestCase):
    def test_new_commands_are_named(self):
        self.assertEqual(COMMAND_NAMES[0x70][0x01], "GetInfo")
        self.assertEqual(DEVICE_NAMES[0xF3], "WifiService")
        self.assertEqual(COMMAND_NAMES[0xF3][0x05], "GetAdapterInfo")
        self.assertEqual(COMMAND_NAMES[0xF3][0x06], "TryJoin")
        self.assertEqual(COMMAND_NAMES[0xF3][0x07], "GetJoinStatus")


if __name__ == "__main__":
    unittest.main()
