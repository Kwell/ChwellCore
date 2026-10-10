"""Real two-gateway ownership, paused/crashed owner recovery and stale RPC fencing."""
import argparse
import concurrent.futures
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request

from test_persistent_cluster import Client
from test_consul_cluster import eventually, free_port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8500')
    parser.add_argument('--mysql-container', required=True)
    args = parser.parse_args()
    token = os.environ['CHWELL_DEMO_TOKEN']
    player = 'fenced-' + str(os.getpid())
    node = 'session-game-' + str(os.getpid())
    ports = [free_port(), free_port()]
    game_port = free_port()
    processes, logs, clients = [], [], []
    paused = None

    def spawn(role, port):
        command = [args.executable, role, args.endpoint]
        command += [node, str(port), 'same-generation'] if role == 'game' else [str(port)]
        log = tempfile.TemporaryFile(mode='w+t')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def connect(index):
        client = Client('127.0.0.1', ports[index])
        clients.append(client)
        return client

    def sql(query):
        return subprocess.check_output(['docker', 'exec', '-e', 'MYSQL_PWD', args.mysql_container,
                                        'mysql', '-uchwell', '-N', '-B', 'chwell', '-e', query],
                                       text=True, timeout=15).strip()

    def lease():
        owner, identity, incarnation, epoch = sql(
            f"SELECT owner,node,incarnation,epoch FROM chwell_session_leases WHERE player='{player}'").split('\t')
        return dict(owner=owner, node=identity, incarnation=incarnation, epoch=epoch)

    def document():
        return sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{player}'")

    def direct(fence):
        payload = struct.pack('!I', 7) + json.dumps(dict(action='advance', player=player,
            cluster_token=os.environ['CHWELL_CLUSTER_TOKEN'], lease=fence)).encode()
        with socket.create_connection(('127.0.0.1', game_port), timeout=10) as connection:
            connection.sendall(struct.pack('!HH', 1, len(payload)) + payload)
            def receive(length):
                data = b''
                while len(data) < length:
                    part = connection.recv(length - len(data))
                    assert part, 'RPC peer closed'
                    data += part
                return data
            _, length = struct.unpack('!HH', receive(4))
            response = receive(length)
            assert response[:4] == payload[:4]
            return json.loads(response[4:])

    try:
        game = spawn('game', game_port)
        gateways = [spawn('gateway', port) for port in ports]
        a, b = [eventually(lambda i=i: connect(i), 'Gateway listening') for i in range(2)]
        for client in (a, b):
            eventually(lambda: node in client.ask('status')['nodes'], 'Game discovered')
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(client.ask, 'login', player=player, token=token) for client in (a, b)]
            results = [future.result(timeout=20) for future in futures]
        assert sum(result['ok'] for result in results) == 1, results
        winning_index = 0 if results[0]['ok'] else 1
        owner, contender = (a, b) if winning_index == 0 else (b, a)
        assert results[1 - winning_index]['error'] == 'already_logged_in', results
        old = lease()
        assert owner.ask('advance')['packet']['fields'] == {'10': 2, '20': 10}
        persisted = document()

        # A paused process retains its TCP connection and local route. DB time
        # expires its authority; the live gateway can take over with a new epoch.
        paused = gateways[winning_index]
        os.kill(paused.pid, signal.SIGSTOP)
        def takeover():
            result = contender.ask('login', player=player, token=token)
            return result if result['ok'] else None
        result = eventually(takeover, 'Expired paused gateway reclaimed', timeout=25)
        assert result['packet']['fields'] == {'1': player, '10': 2, '20': 10}, result
        current = lease()
        assert int(current['epoch']) > int(old['epoch']) and current['owner'] != old['owner']
        assert direct(old)['error'] == 'session_lost'
        assert document() == persisted, 'Stale RPC modified the record'
        os.kill(paused.pid, signal.SIGCONT)
        paused = None
        assert owner.ask('advance')['error'] in ('login_required', 'session_lost')
        assert owner.ask('logout')['ok']
        assert lease() == current, 'Stale renewal/release disturbed replacement'
        assert contender.ask('advance')['packet']['fields'] == {'10': 3, '20': 20}

        # Same node ID and CLI generation, new process incarnation. Requests for
        # the former process must fail even before gateway discovery refresh.
        game.terminate(); assert game.wait(timeout=15) == 0
        game = spawn('game', game_port)
        def new_game_ready():
            with urllib.request.urlopen(args.endpoint + '/v1/catalog/service/persistent-game', timeout=5) as response:
                entries = json.load(response)
            return any(entry['ServiceID'] == node and entry.get('ServiceMeta', {}).get('incarnation') != current['incarnation']
                       for entry in entries)
        eventually(new_game_ready, 'New process identity')
        persisted = document()
        assert direct(current)['error'] == 'session_lost'
        assert document() == persisted
        eventually(lambda: contender.ask('get').get('error') == 'login_required', 'Replacement withdrew old route')
        result = eventually(takeover, 'New incarnation login')
        assert result['packet']['fields']['20'] == 20
        assert lease()['incarnation'] != current['incarnation']

        # Abrupt gateway death cannot release its lease. The other gateway waits
        # for expiry, acquires a higher epoch and loads only committed state.
        killed = gateways[1 - winning_index]
        before_crash = lease()
        killed.kill(); killed.wait(timeout=10)
        def crash_recovery():
            result = owner.ask('login', player=player, token=token)
            return result if result['ok'] else None
        result = eventually(crash_recovery, 'Crashed gateway reclaimed', timeout=25)
        assert result['packet']['fields']['20'] == 20
        assert int(lease()['epoch']) > int(before_crash['epoch'])
        assert direct(before_crash)['error'] == 'session_lost'
        assert document() == persisted
        assert owner.ask('advance')['packet']['fields'] == {'10': 4, '20': 30}
        assert owner.ask('logout')['ok']
        print('PASS: two gateways, simultaneous login, paused/crashed owner takeover, '
              'stale RPC/renew/release and same-generation Game replacement')
    finally:
        if paused is not None and paused.poll() is None:
            os.kill(paused.pid, signal.SIGCONT)
        for client in clients:
            client.close()
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    assert process.wait(timeout=15) == 0
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
        for log in logs:
            log.seek(0)
            output = log.read()
            print(output[-6000:])
            log.close()
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output, output[-6000:]


if __name__ == '__main__':
    main()
