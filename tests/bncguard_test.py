# SPDX-FileCopyrightText: 2019 Snoonet
# SPDX-FileCopyrightText: 2020-present linuxdaemon <linuxdaemon.irc@gmail.com>
#
# SPDX-License-Identifier: MIT

import asyncio
from collections import defaultdict
from contextlib import AbstractContextManager
from pathlib import Path
from unittest import mock

import pytest

from bncbot import bot
from bncbot.config import BNCData, BotConfig
from bncbot.conn import Conn
from bncbot.util import ZNCModuleError


def _make_conn(max_inline_lines: int = 3) -> Conn:
    conn = Conn.__new__(Conn)
    conn.config = BotConfig(
        log_channel="##log", max_inline_lines=max_inline_lines
    )
    conn.run_dir = Path()
    conn.bnc_data = BNCData()
    conn.futures = {}
    conn.locks = defaultdict(asyncio.Lock)
    conn.module_replies = {}
    conn.loop = asyncio.get_running_loop()
    return conn


async def _from_module(conn: Conn, module: str, message: str) -> None:
    await bot.on_privmsg(
        mock.MagicMock(),
        ["admin", message],
        conn,
        f"*{module}",
        "znc.in",
        {},
        False,
    )


def _fake_znc(
    conn: Conn, replies: dict[str, list[str]]
) -> AbstractContextManager[mock.MagicMock]:
    """Answer module commands like ZNC: reply lines, then the PONG.
    replies maps "module command" to its reply lines."""
    pending: list[str] = []
    tasks: list[asyncio.Task[None]] = []

    def send(*parts: str) -> None:
        line = " ".join(parts)
        if line.startswith("PRIVMSG *"):
            target, _, text = line[len("PRIVMSG *") :].partition(" :")
            pending.append(f"{target} {text}")
        elif line.startswith("PING :"):
            key = pending.pop(0)

            async def answer() -> None:
                for reply in replies.get(key, []):
                    await _from_module(conn, key.split()[0], reply)
                await bot.on_pong(conn, ["irc.znc.in", line[6:]])

            tasks.append(conn.loop.create_task(answer()))

    return mock.patch.object(conn, "send", side_effect=send)


def _messages(event: mock.MagicMock) -> list[str]:
    return [call.args[0] for call in event.message.call_args_list]


async def test_announcements_relayed_to_log_channel() -> None:
    conn = _make_conn()
    with mock.patch.object(conn, "send") as mock_send:
        await _from_module(conn, "bncguard", "Blocked bob: inactive 40 days")

    mock_send.assert_called_once_with(
        "PRIVMSG ##log :[bncguard] Blocked bob: inactive 40 days"
    )


async def test_query_module_collects_until_pong() -> None:
    conn = _make_conn()
    with _fake_znc(
        conn, {"bncguard Show a": ["one"], "bncguard List": ["two", "three"]}
    ) as mock_send:
        show, blocked = await asyncio.gather(
            conn.query_module("bncguard", "Show a"),
            conn.query_module("bncguard", "List"),
        )

    assert (show, blocked) == (["one"], ["two", "three"])
    assert conn.module_replies == {}
    assert conn.futures == {}
    # Replies during a query don't leak into the log channel
    assert not [c for c in mock_send.call_args_list if "##log" in c.args[0]]


async def test_query_module_timeout() -> None:
    conn = _make_conn()
    with (
        mock.patch.object(conn, "send"),
        pytest.raises(ZNCModuleError, match="Timed out"),
    ):
        await conn.query_module("bncguard", "List", timeout=0.01)

    assert conn.futures == {}
    assert conn.module_replies == {}


async def test_commands_relay_inline() -> None:
    conn = _make_conn()
    event = mock.MagicMock()
    with _fake_znc(
        conn,
        {
            "bncguard Show alice": ["alice last seen: never"],
            "bncguard Block alice spam bot": ["Blocked alice"],
            "bncguard Unblock alice": ["Unblocked alice"],
            "bncguard Shared alice": ["No other account shared"],
        },
    ):
        await bot.cmd_lastseen("alice", conn, event)
        await bot.cmd_blockbnc("alice  spam bot", conn, event)
        await bot.cmd_unblockbnc("alice", conn, event)
        await bot.cmd_ipshared("alice", conn, event)

    assert _messages(event) == [
        "alice last seen: never",
        "Blocked alice",
        "Unblocked alice",
        "No other account shared",
    ]


async def test_commands_paste_long_replies() -> None:
    conn = _make_conn(max_inline_lines=1)
    event = mock.MagicMock()
    with (
        _fake_znc(
            conn, {"bncguard List": ["a", "b"], "bncguard Users": ["one user"]}
        ),
        mock.patch.object(
            conn, "upload_pastebin", return_value="https://paste/x"
        ) as upload,
    ):
        await bot.cmd_listblocked(conn, event)
        await bot.cmd_listusers(conn, event)

    assert upload.call_args_list[0].args[0] == (
        "Blocked BNC Users\n-----------------\n\na\nb"
    )
    assert _messages(event) == [
        "Blocked BNC Users: https://paste/x",
        "BNC Users: https://paste/x",
    ]


async def test_command_without_module() -> None:
    conn = _make_conn()
    event = mock.MagicMock()
    with _fake_znc(conn, {}):
        await bot.cmd_listblocked(conn, event)

    assert _messages(event) == [
        "No reply from *bncguard (is it loaded globally?)"
    ]
