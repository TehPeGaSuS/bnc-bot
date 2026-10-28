# SPDX-FileCopyrightText: 2019 Snoonet
# SPDX-FileCopyrightText: 2020-present linuxdaemon <linuxdaemon.irc@gmail.com>
#
# SPDX-License-Identifier: MIT

import ipaddress
import re
from collections import defaultdict
from collections.abc import Iterable
from datetime import datetime, timedelta, timezone
from typing import NamedTuple

from bncbot.util import IPAddress, IPNetwork

# Broadcast by ZNC's notify_connect module to admin users as a *status notice:
# "*** <user>[@<clientid>] attached from <ip>"
NOTIFY_CONNECT_RE = re.compile(
    r"^\*\*\* (?P<client>\S+) (?P<event>attached|detached) from (?P<ip>\S+)$"
)


class ClientEvent(NamedTuple):
    client: str
    attached: bool
    ip: IPAddress


def parse_notify_connect(message: str) -> ClientEvent | None:
    """Parse a notify_connect notice, returning None for anything else.

    >>> parse_notify_connect("*** foo@phone attached from ::ffff:192.0.2.1")
    ClientEvent(client='foo@phone', attached=True, ip=IPv4Address('192.0.2.1'))
    >>> parse_notify_connect("*** foo detached from 2001:db8::1")
    ClientEvent(client='foo', attached=False, ip=IPv6Address('2001:db8::1'))
    >>> parse_notify_connect("*** some other broadcast") is None
    True
    """
    match = NOTIFY_CONNECT_RE.match(message.strip())
    if not match:
        return None

    try:
        ip = ipaddress.ip_address(match.group("ip").split("%", 1)[0])
    except ValueError:
        return None

    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped

    return ClientEvent(
        match.group("client"), match.group("event") == "attached", ip
    )


def resolve_username(client: str, known_users: Iterable[str]) -> str:
    """Strip a "@clientid" suffix from a notify_connect client name. ZNC
    usernames may contain "@" themselves, so prefer an exact known match.

    >>> resolve_username("foo@phone", ["foo"])
    'foo'
    >>> resolve_username("a@b", ["a@b"])
    'a@b'
    """
    if client in known_users:
        return client

    return client.rsplit("@", 1)[0]


def ip_prefix(ip: IPAddress, v4_prefix: int, v6_prefix: int) -> IPNetwork:
    """
    >>> ip_prefix(ipaddress.ip_address("2001:db8::1:2:3:4"), 32, 64)
    IPv6Network('2001:db8::/64')
    >>> ip_prefix(ipaddress.ip_address("192.0.2.77"), 24, 64)
    IPv4Network('192.0.2.0/24')
    """
    prefix = v4_prefix if ip.version == 4 else v6_prefix
    return ipaddress.ip_network(f"{ip}/{prefix}", strict=False)


class SharedIPTracker:
    """Track which BNC users are attached from which IP prefix, and report
    other users sharing a prefix - either attached right now, or (with a
    non-zero window) seen on it within that window."""

    def __init__(self, window: timedelta = timedelta(0)) -> None:
        self.window = window
        # prefix -> user -> number of currently attached clients
        self.attached: dict[IPNetwork, dict[str, int]] = defaultdict(dict)
        # prefix -> user -> last time a client attached or detached
        self.last_seen: dict[IPNetwork, dict[str, datetime]] = defaultdict(dict)

    def attach(
        self, user: str, prefix: IPNetwork, now: datetime | None = None
    ) -> set[str]:
        """Record a client attaching, returning other users sharing the
        prefix."""
        now = now or datetime.now(timezone.utc)
        users = self.attached[prefix]
        others = {u for u, count in users.items() if count > 0 and u != user}
        if self.window:
            others.update(
                u
                for u, seen in self.last_seen[prefix].items()
                if u != user and now - seen <= self.window
            )

        users[user] = users.get(user, 0) + 1
        self.last_seen[prefix][user] = now
        self._expire(now)
        return others

    def detach(
        self, user: str, prefix: IPNetwork, now: datetime | None = None
    ) -> None:
        now = now or datetime.now(timezone.utc)
        users = self.attached.get(prefix)
        if users and user in users:
            users[user] -= 1
            if users[user] <= 0:
                del users[user]

            if not users:
                del self.attached[prefix]

        self.last_seen[prefix][user] = now
        self._expire(now)

    def forget(self, user: str) -> None:
        """Drop all state for a user, e.g. once they've been blocked."""
        for table in (self.attached, self.last_seen):
            for prefix in list(table):
                table[prefix].pop(user, None)
                if not table[prefix]:
                    del table[prefix]

    def _expire(self, now: datetime) -> None:
        for prefix in list(self.last_seen):
            seen = self.last_seen[prefix]
            for user in [u for u, t in seen.items() if now - t > self.window]:
                del seen[user]

            if not seen:
                del self.last_seen[prefix]
