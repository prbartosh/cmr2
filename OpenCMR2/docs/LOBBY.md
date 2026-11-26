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

**Return routability.** Before the lobby acts on a registration, lookup or
relay request, the sender proves that it receives packets at its source
address. A request with a missing or wrong cookie gets a `CHALLENGE`
carrying a cookie for that address and port; the client sends the same
request again with the cookie. The lobby keeps no state for unproven
requests, so forged source addresses cannot create sessions, relays or
introductions, or use up another address's rate limits. This is the same
idea as the cookie in DTLS (HelloVerifyRequest) and QUIC (Retry).

The lobby never sees the game itself: once host and guest talk to each
other (directly or through a relay), the lobby only keeps the registration
alive.

Protocol: `src/net/lobby_wire.h` (messages) and `src/net/join_code.h` (code
format). Every request is padded to 64 bytes and every reply, `CHALLENGE`
(26 bytes) included, is shorter than the request, so the lobby cannot be
used to amplify traffic.

### Cookies

`REGISTER`, `REFRESH`, `RESOLVE` and `RELAY` carry a 16-byte cookie after the
nonce. The cookie is SipHash-2-4 with 128-bit output, keyed with 128 random
bits the lobby draws from the system's secure random source at start-up,
over the source IPv4 address, the source port and a 30-second time window.
The lobby accepts the cookie of the current and the previous window (30 to
60 seconds of validity) and compares it in constant time. A restart picks a
new key, so clients simply get one more `CHALLENGE`.

- `REGISTER`, `RESOLVE` and `RELAY` need a valid cookie. Without one, the
  lobby answers `CHALLENGE` and does nothing else: no session, no relay, no
  introduction to the host, and no rate limit or per-address cap is
  consulted.
- `REFRESH` needs a valid cookie only when it comes from another address or
  port than the session's current host address. A host behind a NAT that
  changes its mapping moves the session only after proving it receives at
  the new address; a refresh from the same address works with any cookie.
- `UNREGISTER` needs only the session secret.

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
| `--max-sessions N` | 4096 | Registered hosts at once (at most 65536) |
| `--max-guest-relays N` | 2 | Relays at once requested from one IP address |
| `--max-host-relays N` | 7 | Relays at once to hosts on one IP address, over all their sessions |
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
| Introductions per session | 16 per second |
| Relays per session | 7 |
| Relays requested from one IP address | 2 at once (`--max-guest-relays`) |
| Relays to hosts on one IP address | 7 at once (`--max-host-relays`) |
| Relay binding | both sides within 10 s, or the relay closes |
| Relay bandwidth | 256 KiB/s per relay (bursts of 512 KiB) |
| Relay idle timeout | 30 s without forwarded traffic (binding does not count) |
| Largest datagram | 4096 bytes |

Over a limit the lobby answers `FULL` or `RATE_LIMITED`. All limits are
checked only after the cookie is valid. The rate limit table holds at most
65536 addresses. It never drops an address that is still limited: when the
table is full, it forgets addresses idle for 40 seconds (their limits are
back to full by then), and if it is still full, requests from new
addresses get `RATE_LIMITED` until room frees up. Expired sessions and idle
relays are swept once a second.

The per-address limits count IP addresses, not players. Players behind one
carrier-grade NAT (common on mobile networks and some ISPs) share one
public address and therefore share its registrations, lookups and relays:
for example, a third player behind the same CGNAT address cannot get a relay
while two others hold one. If that matters for your players, raise
`--max-guest-relays` and `--max-host-relays`; the other per-address limits
are fixed in the code (`lobby::Options` and the constants at the top of
`lobby_server.cpp`).

## Security and privacy

- **Codes** are 40 random bits from the system's secure random source
  (`getentropy`), 8 characters of Crockford-style base 32. Even with 4096
  live sessions, guessing any live code at the lookup limit takes about 25
  years per address on average. A code only lets someone ask to join: the
  host's game still decides who plays.
- **Secrets and tokens** (128 bits) prove that a request comes from the
  host, and decide which addresses a relay port is bound to. They are
  compared in constant time. The relay records the guest's IP address
  (proven by its cookie) and the host's current IP address when it is
  created, and accepts the host token only from the host's IP address and
  the guest token only from the guest's (any port, so a NAT that changes
  the port still works). A session owner therefore cannot point a relay at
  a third party with a forged `BIND`. After binding, a relay forwards by
  source address: datagrams from any other address are dropped without an
  answer, but a forged source address that matches a bound player is not
  detected.
- **Cookies** stop forged source addresses from creating sessions or relays,
  making hosts send packets to arbitrary addresses, or exhausting another
  address's limits (see Cookies above).
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
loopback and covers the SipHash-2-4-128 reference vectors, the cookie
round trip (missing, wrong, stale and other-address cookies create no
state; window rollover), registration, resolve and introduction, the local
address rule, refresh and moving a session, expiry, the rate limits and
caps (introductions, relays per guest and per host address, a full rate
limit table), relays (binding from the right address only, rebinding,
forwarding, bandwidth, idle expiry, unbound relays) and random input. The
cases that need a second loopback address (127.0.0.2) run on Linux and are
skipped with a message on macOS.

`tests/net_lobby_e2e.cpp` (`ctest -R net_lobby_e2e`) runs real sessions
against the server on the loopback: a direct and a relayed join by code with
messages both ways, two guests on one code, code input variants and wrong or
malformed codes, unregistering on close, an unreachable lobby and the lookup
rate limit.
