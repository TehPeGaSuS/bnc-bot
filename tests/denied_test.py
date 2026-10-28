# SPDX-FileCopyrightText: 2019 Snoonet
# SPDX-FileCopyrightText: 2020-present linuxdaemon <linuxdaemon.irc@gmail.com>
#
# SPDX-License-Identifier: MIT

from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

from bncbot import bot
from bncbot.config import BNCData, BotConfig, Denial
from bncbot.conn import Conn


def _make_conn(tmp_path: Path, max_inline_lines: int = 3) -> Conn:
    conn = Conn.__new__(Conn)
    conn.config = BotConfig(
        log_channel="##log", max_inline_lines=max_inline_lines
    )
    conn.run_dir = tmp_path
    conn.bnc_data = BNCData(queue={"alice": "2020", "bob": "2021"})
    conn.futures = {}
    return conn


def _messages(event: mock.MagicMock) -> list[str]:
    return [call.args[0] for call in event.message.call_args_list]


async def test_denybnc_records_and_persists(tmp_path: Path) -> None:
    conn = _make_conn(tmp_path)
    event = mock.MagicMock(nick="admin")
    with mock.patch.object(conn, "send"):
        await bot.cmd_denybnc(
            "alice spam  account", event, conn.bnc_queue, conn
        )
        await bot.cmd_denybnc("bob", event, conn.bnc_queue, conn)

    assert "alice" not in conn.bnc_queue
    denial = conn.bnc_denied["alice"][0]
    assert (denial.by, denial.reason) == ("admin", "spam  account")
    assert conn.bnc_denied["bob"][0].reason == ""
    assert BNCData.load_config(conn.data_file).denied == conn.bnc_denied


async def test_deniedbnc_inline_filter_and_clear(tmp_path: Path) -> None:
    conn = _make_conn(tmp_path)
    conn.bnc_data.denied = {
        "alice": [
            Denial(time=datetime(2026, 1, 1, tzinfo=timezone.utc), by="a"),
            Denial(
                time=datetime(2026, 3, 1, tzinfo=timezone.utc),
                by="b",
                reason="again",
            ),
        ],
        "bob": [Denial(time=datetime(2026, 2, 1, tzinfo=timezone.utc), by="a")],
    }
    event = mock.MagicMock()
    await bot.cmd_deniedbnc("", conn, event, conn.bnc_denied)
    assert _messages(event) == [
        "alice - 2026-03-01 00:00 UTC by b: again",
        "bob - 2026-02-01 00:00 UTC by a: (no reason)",
        "alice - 2026-01-01 00:00 UTC by a: (no reason)",
    ]

    event = mock.MagicMock()
    await bot.cmd_deniedbnc("bob", conn, event, conn.bnc_denied)
    assert _messages(event) == ["bob - 2026-02-01 00:00 UTC by a: (no reason)"]

    event = mock.MagicMock()
    await bot.cmd_cleardenied("alice", conn, event)
    await bot.cmd_deniedbnc("alice", conn, event, conn.bnc_denied)
    assert _messages(event) == [
        "Cleared denied BNC requests for alice",
        "alice has no denied BNC requests",
    ]


async def test_deniedbnc_pastes_long_lists(tmp_path: Path) -> None:
    conn = _make_conn(tmp_path, max_inline_lines=1)
    conn.bnc_data.denied = {
        name: [Denial(time=datetime(2026, 1, 1, tzinfo=timezone.utc), by="a")]
        for name in ("alice", "bob")
    }
    event = mock.MagicMock()
    with mock.patch.object(
        conn, "upload_pastebin", return_value="https://paste/x"
    ) as upload:
        await bot.cmd_deniedbnc("", conn, event, conn.bnc_denied)

    assert "alice - 2026-01-01" in upload.call_args.args[0]
    assert _messages(event) == ["Denied BNC requests (2): https://paste/x"]


def testdenial_summary() -> None:
    denials = [
        Denial(time=datetime(2026, 1, 1, tzinfo=timezone.utc), by="a"),
        Denial(
            time=datetime(2026, 3, 1, tzinfo=timezone.utc), by="b", reason="x"
        ),
    ]
    assert bot.denial_summary(denials) == (
        "denied 2x before, last 2026-03-01 by b: x"
    )
