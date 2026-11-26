# Join code NAT test

Runs the real `net::Session` join-by-code path (lobby, hole punching, relay)
between two machines behind two Linux NAT routers, all in Docker on this
machine. Nothing leaves the machine and nothing is published.

```sh
OpenCMR2/tools/nat_test/run.sh
```

Needs Docker (tested with Docker Desktop on macOS arm64). Takes about a
minute. Prints a table and exits non-zero when a scenario does not match.

| Variable | |
| --- | --- |
| `NAT_TEST_LOGS=DIR` | where logs go (default: a new temp directory, printed) |
| `NAT_TEST_ONLY=NAME` | run one scenario, e.g. `1-cone-auto` |
| `KEEP_IMAGE=1` | keep the built image |

## Topology

```
host_a 10.201.0.10 -- nat_a 10.201.0.2 | 198.18.0.11 --+
                                                        +-- wan 198.18.0.0/24 -- lobby 198.18.0.10
guest_b 10.202.0.10 -- nat_b 10.202.0.2 | 198.18.0.12 --+
```

- One image (`Dockerfile`, Debian bookworm-slim) holds `code_client`
  (`code_client.cpp` compiled with `src/net/net_session.cpp`,
  `src/net/rendezvous.cpp` and ENet), `opencmr2-lobby` (compiled from
  `src/lobby`) and the router tools.
- `wan` is an internal Docker network. The LANs are plain bridges with
  Docker's masquerading off: Docker drops traffic for addresses outside an
  internal network's subnet, which a LAN behind a router needs. The clients'
  default route points at their router, and the routers have no default
  route, so the only way out of a LAN is the router's NAT. The run checks
  this in the lobby's `--verbose` log: the lobby must see the routers' wan
  addresses and never a LAN address.
- `router.sh` sets up a router: `MASQUERADE` on the wan side, `FORWARD`
  allows LAN to wan and replies only, `INPUT` drops unsolicited wan traffic
  (as a home router does). `cone` keeps the source port for every
  destination (endpoint-independent mapping, filtering by connection);
  `symmetric` adds `--random-fully`, a new random port for each destination.

## Scenarios

| Scenario | Routers | Guest relay | Expected |
| --- | --- | --- | --- |
| 1-cone-auto | cone, cone | auto | found directly (hole punching), messages ok |
| 2-symmetric-auto | symmetric, symmetric | auto | found through the lobby relay, messages ok |
| 3-symmetric-never | symmetric, symmetric | never | `HOST_UNREACHABLE` |
| 4-cone-always | cone, cone | always | found through the relay, messages ok |

The guest sends 20 guaranteed pings (median round trip is `rtt_ms`), 200
guaranteed and 200 unguaranteed messages, and the host echoes each one with
the same guarantee. A pass needs every guaranteed echo and 90% of the
unguaranteed ones. Scenario 3 shows that direct traversal is impossible with
these routers, so the relay is what makes scenario 2 work.

Each scenario starts from new networks and containers. Its directory holds
the client outputs (`host.out`, `host.err`, `guest.out`, `guest.err`), the
lobby log, and per router the rules with counters, the conntrack table and a
capture of its wan side (`.pcap` and text).

## What it does not cover

Real ISP equipment: carrier-grade NAT with shared address pools, mobile
networks, NATs that change the mapping while a session runs, port-restricted
cone NATs other than Linux conntrack, hairpinning (two players behind one
NAT using the public address), packet loss and latency, IPv6.
