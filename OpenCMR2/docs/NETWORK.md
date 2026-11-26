# Network play

The original used DirectPlay 4. OpenCMR2 keeps the game's network code and
its DirectPlay model (`src/port/net.h`) and implements it over UDP with
[ENet](http://enet.bespin.org) (`third_party/enet`, MIT): `src/net/`.

## Playing

In the game's network menu choose **Internet / LAN (UDP)**. One player hosts
a game; the others see it in the list and join.

| Where the players are | What to do |
| --- | --- |
| Same local network | Nothing: games are found by broadcast. |
| Tailscale (or another VPN) | Nothing with Tailscale: the online machines of the tailnet are searched. With another VPN, list the host in `peers`. |
| Internet, no VPN | The host forwards UDP port 28620 on its router; the others put the host's address in `peers`. |

Only the host has to be reachable. Settings (`[net]` in `opencmr2.ini`):

| Option | Default | |
| --- | --- | --- |
| `port` | 28620 | UDP port games are hosted on and searched at |
| `peers` | | More hosts to search: names or addresses, `host:port` for another port |
| `tailscale` | auto | Search the online machines of `tailscale status` (`0` turns it off) |
| `broadcast` | 1 | Search the local network |

Play with people you trust. The game's own message handling (from 2000)
trusts what it receives; the network layer checks its protocol, sizes and
which machine speaks for which player, but not the game's messages. A VPN
such as Tailscale also keeps strangers out.

## Design

**Topology.** The host is the session's server: every other machine connects
to it and the host relays messages between them. Only the host needs to be
reachable (a forwarded port, a LAN, a VPN); the cost is one extra hop between
two guests, a few milliseconds on a LAN or tailnet, for car states of 32
bytes.

**Channels.** ENet channel 0 is reliable and ordered (the game's guaranteed
messages and the session protocol); channel 1 is unreliable (car states,
which carry their own sequence number). A retransmission never delays
positions.

**Host migration.** The game expects DirectPlay's: when the host leaves, the
earliest joined machine becomes the host (`NET_SYS_HOST`) and the others
reconnect to it with their players; machines that do not reconnect in time
are removed. Every machine knows the others' addresses from the roster, so
they all elect the same host without talking.

**Search.** Asynchronous, as in the original (`DPENUMSESSIONS_ASYNC`): while
the game lists sessions, a query goes out every second (broadcast, `peers`,
Tailscale) and `Net_EnumSessions` returns what has answered. Queries use the
session port through ENet's intercept hook, so one UDP port does everything.
Full sessions are not listed, and at most 64 sessions are kept.

**Amplification.** A reply (up to 342 bytes) is larger than a bare query
(26 bytes), so queries are padded to the largest reply. A host answers a
query from a public address only if the query is at least as large as its
reply; queries from private (RFC 1918), loopback, link-local (169.254/16)
and shared (100.64/10, Tailscale) addresses are answered whatever their
size, so older builds still find games on a LAN or a tailnet.

**Join codes.** The lobby protocol and server are described in
[LOBBY.md](LOBBY.md); the client side lives in `src/net/rendezvous.cpp`.

- The `lobby` setting is `host` or `host:port`, with a port of 1 to 65535
  in digits only. Anything else is logged once and reported as
  `NET_CODE_LOBBY_UNREACHABLE`.
- The lobby's name is resolved on its own thread, again every 10 minutes
  (every 5 seconds while the lobby is unreachable or unresolved). Nothing is
  sent, and no attempt is counted, before it resolves. A search by code
  fails with `NET_CODE_LOBBY_UNREACHABLE` when the name does not resolve or
  the lobby has not answered within 10 seconds; a host reports it and keeps
  trying.
- Every REGISTER, REFRESH, RESOLVE and RELAY carries the last cookie the
  lobby handed out (zero at first). A CHALLENGE from the lobby that names an
  outstanding request stores its cookie and sends that request again with
  the same nonce, at most 3 times per attempt; this is not counted as a
  failed attempt.
- A registered host refreshes every 10 seconds; after 3 refreshes in a row
  without an answer it reports `NET_CODE_LOBBY_UNREACHABLE`, keeps its code
  and keeps refreshing, and is ready again at the next answer.
- A new lobby address is not used while a registration or a search past
  RESOLVED depends on the current one, unless the host's lobby is
  unreachable; it is taken as soon as nothing depends on the old one.
- The search by code queries the host with the application GUID of the
  game's session list, so it starts only once the game has listed sessions
  (`Net_EnumSessions`); until then it reports `NET_CODE_CONTACTING`. It is
  cancelled when the game creates or joins a session.

**Service thread.** A thread services the socket, as DirectPlay's did:
connections stay up while the game loads a stage without polling.
Disconnections are noticed within 10 seconds (5 to 10 seconds of silence).

**Players.** The host numbers machines as they join; machines number their
own players (`machine << 16 | n`), so creating a player needs no round trip.
System messages (players created and destroyed, player data, session
description, host, session lost) are built in DirectPlay's layouts when the
game receives them, with their pointers into the game's buffer, as
DirectPlay did.

**Validation.** Everything read from the network is bounds-checked
(`net_wire.h`). The host refuses a machine that creates, changes, removes or
speaks for a player that is not its own, and drops it. Names are cut to the
game's buffer sizes (100 bytes for player names, which the game copies with
`strcpy`); messages are limited to 256 KB.

## Protocol

Little-endian, version 1. The search, on the session port, outside ENet:

    query  "CMR2" 1 version nonce:u32 application:guid padding
    reply  "CMR2" 2 version nonce:u32 description passwordRequired:u8

Trailing bytes in a query are ignored, so the padding needs no new version.

Sessions, over ENet; the first byte is the opcode:

| Opcode | Direction | Contents |
| --- | --- | --- |
| JOIN | guest to host | version, instance GUID, password |
| RESUME | guest to new host | version, instance GUID, machine |
| ACCEPT | host to guest | description, host and own machine, machines, players |
| REJECT | host to guest | result (DirectPlay's error code) |
| MACHINES | host to all | machines with their addresses |
| PLAYER_ADD, PLAYER_REMOVE, PLAYER_DATA | both ways | a player; the host forwards |
| SESSION_DESC | host to all | description |
| DATA | both ways | origin machine, from, to (0 = all), payload |

## Tests

`tests/net_session.cpp` runs a host and guests on the loopback: search,
passwords, full sessions, player events, 500 guaranteed messages in order,
unguaranteed messages, messages to all, player data, session description,
buffers too small, garbage datagrams, protocol violations, a guest leaving,
host migration and a host that disappears without a word. It also passes
under ThreadSanitizer and AddressSanitizer.

`net_frontend` runs a real UDP host and guest, receiving into memory above
4 GB on 64-bit builds, and calls the game's notification handlers for joins,
player updates, host options and departures.

`tests/lan_game.py` runs two game processes through the real discovery, join
and ready menus, then checks 500 updates, two cars and movement of the remote
car on both machines. It is registered in CTest when original game data is
supplied. A Windows build can take part through Wine:

```sh
python3 tests/lan_game.py --host-probe build/windows-probe \
  --guest-probe build/linux/tests/stage_probe --data /path/to/game \
  --output build/lan-windows-host
```
