#!/usr/bin/env python3
"""Independent game processes (two by default, --players up to 8): LAN discovery,
join, ready and stage load.

Accepts native probes or the Wine wrapper used by physics_parity.py. Both
processes use isolated profiles and the same real UDP protocol as LAN play.
With --join-code the guests know no address: they find the host by typing its
game code, through a real opencmr2-lobby on 127.0.0.1.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import socket
import subprocess
import tempfile
import time


def udp_ports_free(ports):
    sockets = []
    try:
        for port in ports:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(s)
            s.bind(('127.0.0.1', port))
        return True
    except OSError:
        return False
    finally:
        for s in sockets:
            s.close()


def start_lobby(lobby, output, avoid):
    for _ in range(100):
        first = random.randrange(40000, 60000)
        relay_ports = range(first, first + 8)
        if not avoid.intersection(relay_ports) and udp_ports_free(relay_ports):
            break
    else:
        raise RuntimeError('no free relay port range')
    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
        s.close()
        if port not in relay_ports and port not in avoid:
            break
    log = (output / 'lobby.log').open('w')
    process = subprocess.Popen(
        [str(lobby), '--bind', '127.0.0.1', '--port', str(port),
         '--relay-ports', f'{relay_ports[0]}-{relay_ports[-1]}', '--verbose'],
        stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 10
    while 'listening on' not in (output / 'lobby.log').read_text():
        if process.poll() is not None or time.monotonic() > deadline:
            process.kill()
            process.wait()
            log.close()
            raise RuntimeError(f'lobby did not start: {output / "lobby.log"}')
        time.sleep(0.05)
    return process, log, port


def check_join_code(output, roles, relay):
    host_log = (output / 'host/user/opencmr2.log').read_text()
    codes = re.findall(r'^net: join code (\S+)$', host_log, re.M)
    if len(codes) != 1:
        raise ValueError(f'host logged {len(codes)} join codes, expected 1')
    code = codes[0]
    lobby_log = (output / 'lobby.log').read_text()
    path = 'relay' if relay == 'always' else 'direct'
    for role in roles[1:]:
        if f'probe: typed join code {code}' not in (output / role / 'run.log').read_text():
            raise ValueError(f'{role} did not type the code {code}')
        guest_log = (output / role / 'user/opencmr2.log').read_text()
        found = re.findall(r'^net: game found by code \((\w+)\)$', guest_log, re.M)
        if found != [path]:
            raise ValueError(f'{role} found the game by code {found}, expected [{path!r}]')
        if 'net: joined "' not in guest_log:
            raise ValueError(f'{role} did not join the session')
    resolved = lobby_log.count(f'session {code} resolved by')
    if resolved < len(roles) - 1:
        raise ValueError(f'lobby resolved {code} {resolved} times for {len(roles) - 1} guests')
    relays = lobby_log.count(f'session {code} relay on port')
    if relay == 'always' and relays < len(roles) - 1:
        raise ValueError(f'lobby allocated {relays} relays for {len(roles) - 1} guests')
    return {'code': code, 'path': path, 'lobby_resolved': resolved, 'lobby_relays': relays}


def verify(host_probe, guest_probe, data, output, players=2, seconds=40, lobby=None, relay=None):
    roles = ['host'] + ([f'guest{i}' for i in range(1, players)] if players > 2 else ['guest'])
    processes = []
    logs = []
    lobby_process = None
    # Reserve distinct ports until profiles are written.
    sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in roles]
    try:
        for s in sockets:
            s.bind(('127.0.0.1', 0))
        ports = [s.getsockname()[1] for s in sockets]
        host_port = ports[0]
        if lobby:
            lobby_process, lobby_log, lobby_port = start_lobby(lobby, output, set(ports))
            logs.append(lobby_log)
        for index, (role, port) in enumerate(zip(roles, ports)):
            probe = host_probe if role == 'host' else guest_probe
            run = output / role
            user = run / 'user'
            user.mkdir(parents=True)
            if lobby:
                net = f'peers=\nlobby=127.0.0.1:{lobby_port}\n'
                if relay and role != 'host':
                    net += f'relay={relay}\n'
            else:
                net = f'peers=127.0.0.1:{host_port}\n'
            (user / 'opencmr2.ini').write_text(f'[net]\nport={port}\n{net}broadcast=0\ntailscale=off\n')
            env = {k: v for k, v in os.environ.items() if not k.startswith('OPENCMR2_PROBE_')}
            browser = 1 if role == 'host' else 2 if lobby else 0
            if lobby and role != 'host':
                env['OPENCMR2_PROBE_JOIN_CODE_LOG'] = str(output / 'host/user/opencmr2.log')
            env.update(OPENCMR2_HEADLESS='1', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                       XDG_DATA_HOME=str(user), OPENCMR2_PROBE_LAN_ROLE='host' if role == 'host' else 'guest',
                       OPENCMR2_PROBE_LAN_GUESTS=str(players - 1), OPENCMR2_PROBE_LAN_SECONDS=str(seconds),
                       OPENCMR2_PROBE_CURSORS=f'0:3,33:2,60:0,53:{browser},61:6,19:2,62:3',
                       OPENCMR2_PROBE_EXPECT_CARS=str(players), OPENCMR2_PROBE_EXPECT_MODE='8',
                       OPENCMR2_PROBE_SESSION_NAME='LAN TEST')
            logs.append((run / 'run.log').open('w'))
            sockets[index].close()
            processes.append(subprocess.Popen(
                [str(probe), str(data), str(run / 'trace.jsonl'), '30', '500'],
                env=env, stdout=logs[-1], stderr=subprocess.STDOUT))
        for role, process in zip(roles, processes):
            code = process.wait(timeout=seconds + 20 + 15 * players)
            if code:
                raise RuntimeError(f'{role} failed ({code}): {output / role / "run.log"}')
        report = {}
        for role in roles:
            records = [json.loads(line) for line in (output / role / 'trace.jsonl').read_text().splitlines()]
            if len(records) != 500 or records[-1]['driving_tick'] <= 0:
                raise ValueError(f'{role}: no complete {players}-player race')
            # Every other car moves on every machine.
            positions = {}
            for record in records:
                if record['state'] == 8:
                    for car in record['cars']:
                        positions.setdefault(car['id'], set()).add(tuple(car['position']))
            if len(positions) != players:
                raise ValueError(f'{role}: {len(positions)} cars, expected {players}')
            remote = {car: len(seen) for car, seen in positions.items() if car != 0}
            if min(remote.values()) < 50:
                raise ValueError(f'{role}: a remote car did not advance ({remote})')
            report[role] = {'ticks': len(records), 'driving_ticks': records[-1]['driving_tick'],
                            'distinct_remote_positions': remote}
        if 'probe: LAN guest ready' not in (output / 'host/run.log').read_text():
            raise ValueError('host started without receiving guest readiness')
        if lobby:
            report['join_code'] = check_join_code(output, roles, relay)
        (output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
        how = f' by join code ({report["join_code"]["path"]})' if lobby else ''
        print(f'LAN game: {players} machines joined{how} and drove 500 ticks with {players} cars')
    finally:
        if lobby_process is not None:
            lobby_process.terminate()
            try:
                lobby_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                lobby_process.kill()
                lobby_process.wait()
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.close()
        for s in sockets:
            s.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-probe', type=Path, required=True)
    parser.add_argument('--guest-probe', type=Path)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--players', type=int, default=2, choices=range(2, 9))
    parser.add_argument('--seconds', type=int, default=40, help='wall time per process (more under the sanitizers)')
    parser.add_argument('--join-code', action='store_true', help='find the host by its game code through a local lobby')
    parser.add_argument('--lobby', type=Path, help='opencmr2-lobby binary (with --join-code)')
    parser.add_argument('--relay', choices=['auto', 'always', 'never'], help='[net] relay of the guests (with --join-code)')
    args = parser.parse_args()
    if args.join_code != bool(args.lobby) or (args.relay and not args.join_code):
        parser.error('--join-code needs --lobby, and --lobby and --relay need --join-code')
    lobby = args.lobby.resolve() if args.lobby else None
    host = args.host_probe.resolve()
    guest = (args.guest_probe or args.host_probe).resolve()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        verify(host, guest, args.data.resolve(), args.output.resolve(), args.players, args.seconds, lobby, args.relay)
    else:
        with tempfile.TemporaryDirectory(prefix='opencmr2-lan-') as directory:
            verify(host, guest, args.data.resolve(), Path(directory), args.players, args.seconds, lobby, args.relay)


if __name__ == '__main__':
    main()
