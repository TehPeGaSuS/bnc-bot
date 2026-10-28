# SPDX-FileCopyrightText: 2019 Snoonet
# SPDX-FileCopyrightText: 2020-present linuxdaemon <linuxdaemon.irc@gmail.com>
#
# SPDX-License-Identifier: MIT

from pathlib import Path
from unittest import mock

from bncbot import bot
from bncbot.config import BNCData, BotConfig
from bncbot.conn import Conn


def _make_conn() -> Conn:
    conn = Conn.__new__(Conn)
    conn.config = BotConfig(log_channel="##log")
    conn.run_dir = Path()
    conn.bnc_data = BNCData()
    conn.futures = {}
    return conn


async def test_ipguard_messages_relayed_to_log_channel() -> None:
    conn = _make_conn()
    with mock.patch.object(conn, "send") as mock_send:
        await bot.on_privmsg(
            mock.MagicMock(),
            ["BNCServ", "Blocked bob (192.0.2.1), carol (192.0.2.1): shared"],
            conn,
            "*ipguard",
            "znc.in",
            {},
            False,
        )

    mock_send.assert_called_once_with(
        "PRIVMSG ##log :[ipguard] Blocked bob (192.0.2.1), carol "
        "(192.0.2.1): shared"
    )


async def test_ip_commands_message_ipguard() -> None:
    conn = _make_conn()
    event = mock.MagicMock()
    with mock.patch.object(conn, "send") as mock_send:
        await bot.cmd_ipunblock("alice", conn, event)
        await bot.cmd_ipblocked(conn, event)
        await bot.cmd_ipshow("alice", conn, event)
        await bot.cmd_ipshared("alice", conn, event)

    assert [call.args[0] for call in mock_send.call_args_list] == [
        "PRIVMSG *ipguard :Unblock alice",
        "PRIVMSG *ipguard :List",
        "PRIVMSG *ipguard :Show alice",
        "PRIVMSG *ipguard :Shared alice",
    ]
