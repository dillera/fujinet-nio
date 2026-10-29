from __future__ import annotations

import struct
import unittest

from fujinet_tools import fileproto as fp


def _entry(name: bytes, flags: int, size: int, mtime: int) -> bytes:
    return bytes([flags, len(name)]) + name + struct.pack("<II", size, mtime)


def _page(entries: list[bytes]) -> bytes:
    data = b"".join(entries)
    return bytes([len(entries)]) + struct.pack("<H", len(data)) + data


class TestListPagesProto(unittest.TestCase):
    def test_command_id(self) -> None:
        self.assertEqual(fp.CMD_LIST_PAGES, 0x07)

    def test_request_layout(self) -> None:
        req = fp.build_list_pages_req(
            "sd0:/x",
            3,
            8,
            480,
            list_flags=fp.PAGES_FLAG_SORT_BY_NAME | fp.PAGES_FLAG_DIRS_FIRST,
            max_name_bytes=30,
        )
        self.assertEqual(
            req,
            b"\x01\x06\x00sd0:/x" + b"\x03\x00" + b"\x08" + b"\xe0\x01" + b"\x03" + b"\x1e",
        )

    def test_request_validation(self) -> None:
        with self.assertRaises(ValueError):
            fp.build_list_pages_req("sd0:/", 0, 0, 480)
        with self.assertRaises(ValueError):
            fp.build_list_pages_req("sd0:/", 0, 65, 480)
        with self.assertRaises(ValueError):
            fp.build_list_pages_req("sd0:/", 0x10000, 8, 480)
        with self.assertRaises(ValueError):
            fp.build_list_pages_req("sd0:/", 0, 8, 0)

    def test_response_round_trip(self) -> None:
        payload = (
            b"\x01"
            + bytes([fp.PAGES_RESP_MORE | fp.PAGES_RESP_NAME_CUT])
            + b"\x00\x00"
            + struct.pack("<IHBB", 266, 2, 2, 2)
            + _page(
                [
                    _entry(b"games", fp.PAGES_ENTRY_DIR, 0, 100),
                    _entry(b"abc.atr", 0, 92160, 200),
                ]
            )
            + _page([_entry(b"long_na", fp.PAGES_ENTRY_NAME_CUT, 5, 0)])
        )
        r = fp.parse_list_pages_resp(payload)
        self.assertTrue(r.more)
        self.assertTrue(r.name_cut)
        self.assertEqual((r.total_entries, r.start_page, r.page_size, r.page_count), (266, 2, 2, 2))
        self.assertEqual(r.pages[0][0].name, "games")
        self.assertTrue(r.pages[0][0].is_dir)
        self.assertEqual(r.pages[0][1].size_bytes, 92160)
        self.assertEqual(r.pages[0][1].mtime_unix, 200)
        self.assertTrue(r.pages[1][0].name_cut)

    def test_empty_response_and_errors(self) -> None:
        empty = b"\x01\x01\x00\x00" + struct.pack("<IHBB", 20, 0, 6, 0)
        r = fp.parse_list_pages_resp(empty)
        self.assertEqual(r.page_count, 0)
        self.assertTrue(r.more)
        with self.assertRaises(ValueError):
            fp.parse_list_pages_resp(b"\x02" + empty[1:])
        with self.assertRaises(ValueError):
            fp.parse_list_pages_resp(empty + b"\x00")  # trailing byte
        bad = b"\x01\x00\x00\x00" + struct.pack("<IHBB", 1, 0, 1, 1) + b"\x01\xff\x00"
        with self.assertRaises(ValueError):
            fp.parse_list_pages_resp(bad)


if __name__ == "__main__":
    unittest.main()
