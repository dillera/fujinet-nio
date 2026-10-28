from __future__ import annotations

import unittest
from unittest import mock

from fujinet_tools import net_tcp, netproto as np


class KeepFlagTests(unittest.TestCase):
    """The keep flag (open bit5): exempt from idle reaping and eviction until closed."""

    def test_flag_value(self) -> None:
        self.assertEqual(np.FLAG_KEEP, 0x20)

    def _open_flags(self, **kwargs: object) -> int:
        captured: dict[str, bytes] = {}

        def fake_send_retry(**kw: object) -> None:
            captured["payload"] = kw["payload"]  # type: ignore[assignment]
            return None  # no response: tcp_open raises after the request is built

        with mock.patch.object(net_tcp, "_send_retry", fake_send_retry):
            with self.assertRaises(RuntimeError):
                net_tcp.tcp_open(
                    bus=None,  # type: ignore[arg-type]
                    url="tcp://h:1",
                    timeout=0.0,
                    wait_connected=False,
                    info_poll_s=0.0,
                    **kwargs,  # type: ignore[arg-type]
                )
        # payload: version, method, flags, ...
        return captured["payload"][2]

    def test_tcp_open_is_not_keep_by_default(self) -> None:
        self.assertEqual(self._open_flags() & np.FLAG_KEEP, 0)

    def test_tcp_open_can_ask_for_keep(self) -> None:
        flags = self._open_flags(keep=True)
        self.assertTrue(flags & np.FLAG_KEEP)
        self.assertTrue(flags & np.FLAG_ALLOW_EVICT)  # unchanged: it may evict others, never be evicted


if __name__ == "__main__":
    unittest.main()
