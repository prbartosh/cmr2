# Lobby server (join codes)

`opencmr2-lobby` lets players join an internet game with a short code such
as `7KQ2-M9XA` instead of an address and a forwarded port. It is a small,
single-threaded UDP server (`src/lobby/`) with no dependencies beyond POSIX
and a C++17 compiler. It runs on Linux and macOS.

## What it does

1. **Rendezvous.** A host registers with the lobby and gets a random code
   and a secret. The lobby remembers the host's public address (as seen from
   the lobby) and its local address. The host refreshes the registration
   every 10 seconds; a session the host stops refreshing expires after 35
   seconds.
2. **Hole punching.** A guest resolves the code. The lobby answers with the
   host's public address and, at the same moment, tells the host the guest's
   public address. Both sides then send packets to each other, which opens
   the mapping in most home routers (NAT). Local (LAN) addresses are passed
   on only when host and guest come from the same public address and the
   local address is private (10/8, 172.16/12, 192.168/16): two players behind
   the same router then connect directly over the LAN.
3. **Relay fallback.** When the routers do not let packets through
   (symmetric NAT, carrier-grade NAT), the guest asks for a relay. The lobby
   opens a UDP port from its relay range and gives each side a token. Both
   sides bind to the port with their token, and from then on the lobby
   forwards datagrams between the two, unchanged. The relay does not look
   into the game traffic (ENet), and it is neither encrypted nor
   authenticated: the lobby operator, or anyone on the path, can read it.

The lobby never sees the game itself: once host and guest talk to each
other (directly or through a relay), the lobby only keeps the registration
alive.

Protocol: `src/net/lobby_wire.h` (messages) and `src/net/join_code.h` (code
format). Every request is padded to 64 bytes and every reply is shorter than
the request, so the lobby cannot be used to amplify traffic.

## Firewall

Open these to the internet (UDP only):

| Port | Purpose |
| --- | --- |
| 28630/udp | Lobby (`--port`) |
| 28631-28694/udp | Relays (`--relay-ports`), one port per relayed guest |

No TCP ports are used. The server needs no privileges (all ports are above
1024).

## Running

### Binary

Build without the rest of the project (any C++17 compiler, CMake 3.19+):

```sh
cmake -S OpenCMR2/src/lobby -B build/lobby -G Ninja
cmake --build build/lobby
sudo cmake --install build/lobby          # installs /usr/local/bin/opencmr2-lobby
opencmr2-lobby
```

Inside the full project build the binary is
`build/<preset>/src/lobby/opencmr2-lobby`.

### Docker

The build context is `OpenCMR2/src`:

```sh
docker build -f OpenCMR2/src/lobby/Dockerfile OpenCMR2/src -t opencmr2-lobby
docker run -d --name opencmr2-lobby --restart unless-stopped --network host opencmr2-lobby
```

The image runs as a non-root user on a distroless base. Use host networking
on Linux: with published ports (`-p`) Docker may rewrite the players' source
addresses, and the lobby needs the real ones for hole punching. Arguments
after the image name are added after the built-in `--relay-ports
28631-28694`; a later `--relay-ports` wins.

### systemd

```sh
sudo install -m 755 build/lobby/opencmr2-lobby /usr/local/bin/
sudo install -m 644 OpenCMR2/src/lobby/opencmr2-lobby.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now opencmr2-lobby
journalctl -u opencmr2-lobby
```

The unit runs the server as a dynamic user with a read-only system, no
capabilities, IPv4 sockets only and a system call filter, and restarts it if
it fails.

## Options

| Option | Default | |
| --- | --- | --- |
| `--bind ADDR` | `0.0.0.0` | IPv4 address to listen on |
| `--port N` | 28630 | Lobby UDP port |
| `--relay-ports FIRST-LAST` | 28631-28694 | Relay UDP ports; the number of ports is the number of relays at once |
| `--max-sessions N` | 4096 | Registered hosts at once |
| `--verbose` | off | Log addresses and codes (see Privacy) |
| `--help` | | Show the options |

Invalid arguments stop the server with exit code 2. SIGINT and SIGTERM stop
it cleanly. Logs go to stderr with UTC timestamps.

## Limits

| Limit | Value |
| --- | --- |
| Session lifetime without a refresh | 35 s |
| Sessions per public IP address | 4 |
| Registrations per IP address | 4 at once, then 1 per 10 s |
| Lookups (resolve and relay requests) per IP address | 10 at once, then 1 per 3 s |
| Introductions per session | 4 per second |
| Relays per session | 7 |
| Relay bandwidth | 256 KiB/s per relay (bursts of 512 KiB) |
| Relay idle timeout | 30 s without traffic |
| Largest datagram | 4096 bytes |

Over a limit the lobby answers `FULL` or `RATE_LIMITED`. The rate limit
table holds at most 65536 addresses and forgets idle ones, so spoofed
sources cannot grow it without bound.

## Security and privacy

- **Codes** are 40 random bits from the system's secure random source
  (`getentropy`), 8 characters of Crockford-style base 32. Even with 4096
  live sessions, guessing any live code at the lookup limit takes about 25
  years per address on average. A code only lets someone ask to join: the
  host's game still decides who plays.
- **Secrets and tokens** (128 bits) prove that a request comes from the
  host, and decide which addresses a relay port is bound to. They are
  compared in constant time. After binding, a relay forwards by source
  address: datagrams from any other address are dropped without an answer,
  but a forged source address that matches a bound player is not detected.
- **Malformed input** is dropped silently. The parser checks every length;
  the test suite sends it 20000 random datagrams.
- **What is stored:** only in memory, for the lifetime of a session: the
  code, the secret, the host's public and local address and the game's
  instance id. Nothing is written to disk.
- **What is logged:** by default only the start-up settings, counts
  (sessions, relays, requests, refusals) once a minute, relay byte totals
  when a relay closes, and errors. No IP addresses, codes or secrets.
  `--verbose` adds addresses and codes for troubleshooting; leave it off in
  normal operation (personal data, GDPR).
- **Who sees what:** a guest learns the host's public address (needed to
  connect) and the host learns the guest's. A relay hides neither from the
  lobby operator.

## Pointing the game at a lobby

The game looks for the lobby in `[net]` in `opencmr2.ini`:

```ini
[net]
lobby = lobby.example.org
```

`host:port` selects another port than 28630.

A build can bake a default in with `-DOPENCMR2_LOBBY=lobby.example.org` at
configure time; the ini setting overrides it. Without a lobby the join code
functions (`Net_GetHostCode`, `Net_FindByCode` in `src/port/net.h`) return
`NET_CODE_NO_LOBBY`; games are still found by broadcast, Tailscale and
`peers` as described in [NETWORK.md](NETWORK.md).

## Tests

`tests/lobby_server.cpp` (`ctest -R lobby_server`) runs the server on the
loopback and covers registration, resolve and introduction, the local
address rule, refresh, expiry, the rate limits and caps, relays (binding,
rebinding, forwarding, bandwidth, idle expiry) and random input.
