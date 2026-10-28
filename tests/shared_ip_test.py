# SPDX-FileCopyrightText: 2019 Snoonet
# SPDX-FileCopyrightText: 2020-present linuxdaemon <linuxdaemon.irc@gmail.com>
#
# SPDX-License-Identifier: MIT

import ipaddress
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

from bncbot import shared_ip
from bncbot.config import BNCData, BotConfig
from bncbot.conn import Conn

NET_A = ipaddress.ip_network("2001:db8:0:1::/64")
NET_B = ipaddress.ip_network("2001:db8:0:2::/64")
T0 = datetime(2026, 1, 1, tzinfo=timezone.utc)


def test_tracker_concurrent_only() -> None:
    tracker = shared_ip.SharedIPTracker()
    assert tracker.attach("alice", NET_A, T0) == set()
    assert tracker.attach("alice", NET_A, T0) == set()
    assert tracker.attach("bob", NET_B, T0) == set()
    assert tracker.attach("carol", NET_A, T0) == {"alice"}


def test_tracker_detach_clears_without_window() -> None:
    tracker = shared_ip.SharedIPTracker()
    tracker.attach("alice", NET_A, T0)
    tracker.attach("alice", NET_A, T0)
    tracker.detach("alice", NET_A, T0)
    assert tracker.attach("bob", NET_A, T0) == {"alice"}
    tracker.detach("alice", NET_A, T0)
    tracker.detach("bob", NET_A, T0)
    assert tracker.attach("carol", NET_A, T0) == set()


def test_tracker_window() -> None:
    tracker = shared_ip.SharedIPTracker(timedelta(hours=1))
    tracker.attach("alice", NET_A, T0)
    tracker.detach("alice", NET_A, T0)
    assert tracker.attach("bob", NET_A, T0 + timedelta(minutes=59)) == {"alice"}
    tracker.forget("bob")
    assert tracker.attach("carol", NET_A, T0 + timedelta(hours=2)) == set()


def _make_conn(config: BotConfig, run_dir: Path) -> Conn:
    conn = Conn.__new__(Conn)
    conn.config = config
    conn.run_dir = run_dir
    conn.bnc_data = BNCData(users={"alice": None, "bob": None})
    conn.shared_ip = shared_ip.SharedIPTracker()
    return conn


def _event(client: str, ip: str, attached: bool = True) -> str:
    return f"*** {client} {'attached' if attached else 'detached'} from {ip}"


def _feed(conn: Conn, *messages: str) -> list[str]:
    with mock.patch.object(conn, "send") as mock_send:
        for message in messages:
            event = shared_ip.parse_notify_connect(message)
            assert event
            conn.handle_client_event(event)

    return [call.args[0] for call in mock_send.call_args_list]


def test_conn_blocks_both_on_same_64(tmp_path: Path) -> None:
    conn = _make_conn(BotConfig(block_shared_ip=True), tmp_path)
    sent = _feed(
        conn,
        _event("alice@phone", "2001:db8:0:1::10"),
        _event("bob", "2001:db8:0:1:ffff::1"),
    )
    assert "PRIVMSG *blockuser :Block alice" in sent
    assert "PRIVMSG *blockuser :Block bob" in sent


def test_conn_ipv4_mapped_and_disabled(tmp_path: Path) -> None:
    conn = _make_conn(BotConfig(block_shared_ip=True), tmp_path)
    sent = _feed(
        conn, _event("alice", "::ffff:192.0.2.1"), _event("bob", "192.0.2.1")
    )
    assert "PRIVMSG *blockuser :Block bob" in sent

    conn = _make_conn(BotConfig(), tmp_path)
    assert not _feed(
        conn, _event("alice", "192.0.2.1"), _event("bob", "192.0.2.1")
    )


def test_conn_exemptions(tmp_path: Path) -> None:
    conn = _make_conn(
        BotConfig(
            block_shared_ip=True,
            shared_ip_exempt_users=["Alice"],
            shared_ip_exempt_nets=["198.51.100.0/24"],
        ),
        tmp_path,
    )
    sent = _feed(
        conn,
        _event("alice", "192.0.2.1"),
        _event("bob", "192.0.2.1"),
        _event("bob", "198.51.100.5"),
        _event("carol", "198.51.100.5"),
    )
    assert not [line for line in sent if "blockuser" in line]
