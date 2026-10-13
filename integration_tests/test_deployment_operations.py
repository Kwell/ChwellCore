"""Readiness, liveness, metrics, bounded operations IO and accepted-work drain with real dependencies."""
import argparse
import concurrent.futures
import json
import os
import pathlib
import queue
import signal
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

from test_consul_cluster import eventually, free_port
from test_persistent_cluster import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8500')
    parser.add_argument('--mysql-container', required=True)
    parser.add_argument('--consul-container', required=True)
    args = parser.parse_args()
    executable = str(pathlib.Path(args.executable).resolve())
    processes, logs, clients, slow_peers, lockers = [], [], [], [], []
    prefix = 'deploy-' + str(os.getpid())

    def spawn(role, identity=None, drain_seconds=8):
        port, ops = free_port(), free_port()
        command = [executable, role, args.endpoint]
        command += [identity, str(port), 'v1'] if identity else [str(port)]
        log = tempfile.TemporaryFile(mode='w+t')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   env=dict(os.environ, CHWELL_OPS_PORT=str(ops), CHWELL_DRAIN_SECONDS=str(drain_seconds)))
        processes.append(process)
        return process, port, ops

    def http(port, path):
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/{path}', timeout=3) as reply:
                return reply.status, reply.read().decode()
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode()

    def health(port, ready=True):
        status, body = http(port, 'ready' if ready else 'live')
        data = json.loads(body)
        assert data['ok'] == (status == 200), (status, data)
        return status, data

    def metric(port, name):
        status, text = http(port, 'metrics')
        assert status == 200
        for line in text.splitlines():
            if line.startswith(name + ' '):
                return float(line.split()[1])
        raise AssertionError(f'Missing metric {name}: {text}')

    def docker(*command):
        return subprocess.check_output(['docker', *command], text=True, timeout=45).strip()

    def sql(statement):
        return docker('exec', '-e', 'MYSQL_PWD', args.mysql_container, 'mysql',
                      '--protocol=TCP', '-h127.0.0.1', '-uchwell', '-N', '-B', 'chwell', '-e', statement)

    def lock_document(player):
        locker = subprocess.Popen(['docker', 'exec', '-i', '-e', 'MYSQL_PWD', args.mysql_container,
                                   'mysql', '--protocol=TCP', '-h127.0.0.1', '-uchwell', '-N', '-B',
                                   '--unbuffered', 'chwell'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        lockers.append(locker)
        lines = queue.Queue()
        def read():
            for line in locker.stdout:
                lines.put(line.strip())
        threading.Thread(target=read, daemon=True).start()
        locker.stdin.write(f"START TRANSACTION; SELECT k FROM cluster_kv WHERE k='cluster_players:{player}' FOR UPDATE; SELECT 'locked';\n")
        locker.stdin.flush()
        def locked():
            while not lines.empty():
                if lines.get_nowait() == 'locked':
                    return True
            return False
        eventually(locked, 'External document lock acquired')
        return locker

    def unlock(locker):
        locker.stdin.write('COMMIT;\n')
        locker.stdin.close()
        assert locker.wait(timeout=5) == 0, locker.stderr.read()

    def fast(predicate, description):
        # Observe the blocked work without consuming its two-second SQL lock budget.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.01)
        raise AssertionError(description)

    def connect(port):
        client = Client('127.0.0.1', port)
        clients.append(client)
        return client

    def stop(process):
        if process.poll() is None:
            process.terminate()
            assert process.wait(timeout=25) == 0

    try:
        # Process health is independent of having a usable route.
        gateway, gateway_port, gateway_ops = spawn('gateway')
        eventually(lambda: health(gateway_ops, False)[0] == 200, 'Gateway alive')
        assert health(gateway_ops)[0] == 503
        game, _, game_ops = spawn('game', prefix + '-game')
        eventually(lambda: health(game_ops)[0] == 200 and health(gateway_ops)[0] == 200, 'Dependencies ready')
        assert subprocess.run([executable, 'probe', str(gateway_ops), 'ready'], timeout=4).returncode == 0
        assert metric(gateway_ops, 'chwell_ready') == 1
        assert metric(gateway_ops, 'chwell_game_nodes') >= 1

        # An incomplete HTTP header cannot occupy the operations listener indefinitely.
        for _ in range(4):
            peer = socket.create_connection(('127.0.0.1', gateway_ops), timeout=3)
            peer.sendall(b'GET /live HTTP/1.1\r\n')
            slow_peers.append(peer)
        assert health(gateway_ops, False)[0] == 200
        for peer in slow_peers:
            peer.settimeout(4)
            assert peer.recv(1) == b''
            peer.close()
        slow_peers.clear()
        assert http(gateway_ops, 'unknown')[0] == 400

        # Dependency failures withdraw readiness without causing a restart loop.
        docker('stop', '-t', '1', args.consul_container)
        eventually(lambda: health(gateway_ops)[0] == 503 and health(game_ops)[0] == 503, 'Discovery unready')
        assert health(gateway_ops, False)[0] == 200 and health(game_ops, False)[0] == 200
        docker('start', args.consul_container)
        eventually(lambda: health(game_ops)[0] == 200 and health(gateway_ops)[0] == 200, 'Discovery readiness recovery', timeout=45)
        docker('stop', '-t', '1', args.mysql_container)
        eventually(lambda: health(game_ops)[0] == 503 and health(gateway_ops)[0] == 503, 'Database unready')
        assert health(gateway_ops, False)[0] == 200 and health(game_ops, False)[0] == 200
        docker('start', args.mysql_container)
        eventually(lambda: health(game_ops)[0] == 200 and health(gateway_ops)[0] == 200, 'Database readiness recovery', timeout=60)

        player = prefix + '-player'
        owner = connect(gateway_port)
        assert owner.ask('login', player=player, token=os.environ['CHWELL_DEMO_TOKEN'])['ok']
        assert metric(gateway_ops, 'chwell_sessions') == 1
        # Block a mutation behind a separately held document lock; a lease lock
        # could be consumed by the gateway's periodic renewal before its RPC.
        # The server lock wait is 2s; observe worker entry, enqueue status, then drain.
        locker = lock_document(player)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            mutation = pool.submit(owner.ask, 'advance')
            fast(lambda: metric(game_ops, 'chwell_inflight_requests') == 1 and metric(gateway_ops, 'chwell_inflight_requests') == 1,
                       'Mutation executing before drain')
            # Queue another accepted request on another stream before admission closes.
            queued = connect(gateway_port)
            payload = json.dumps({'action': 'status', 'sync_version': 1}).encode()
            import struct
            queued.socket.sendall(struct.pack('!HH', 1, len(payload)) + payload)
            fast(lambda: metric(gateway_ops, 'chwell_queued_inputs') >= 1, 'Accepted request queued')
            gateway.send_signal(signal.SIGUSR1)
            eventually(lambda: health(gateway_ops)[1]['draining'], 'Readiness withdrawn before drain')
            assert health(gateway_ops)[0] == 503 and health(gateway_ops, False)[0] == 200
            unlock(locker)
            result = mutation.result(timeout=10)
            assert result['ok'] and result['packet']['fields'] == {'10': 2, '20': 10}, result
            _, size = struct.unpack('!HH', queued.receive(4))
            assert json.loads(queued.receive(size))['ok']
        eventually(lambda: health(gateway_ops)[1]['drained'], 'Accepted work drained')
        assert metric(gateway_ops, 'chwell_drain_dropped_inputs_total') == 0
        assert metric(gateway_ops, 'chwell_inflight_requests') == 0
        assert metric(gateway_ops, 'chwell_sessions') == 0
        rejected = metric(gateway_ops, 'chwell_rejected_inputs_total')
        queued.socket.sendall(struct.pack('!HH', 1, len(payload)) + payload)
        eventually(lambda: metric(gateway_ops, 'chwell_rejected_inputs_total') > rejected, 'Post-drain admission rejected')
        assert subprocess.run([executable, 'probe', str(gateway_ops), 'ready'], timeout=4).returncode == 1
        assert subprocess.run([executable, 'probe', str(gateway_ops), 'live'], timeout=4).returncode == 0
        stop(gateway)

        # Replacement gateway recovers committed data and can acquire the released lease.
        replacement, replacement_port, replacement_ops = spawn('gateway')
        eventually(lambda: health(replacement_ops)[0] == 200, 'Replacement gateway ready')
        recovered = connect(replacement_port)
        result = recovered.ask('login', player=player, token=os.environ['CHWELL_DEMO_TOKEN'])
        assert result['ok'] and result['packet']['fields']['20'] == 10, result
        stop(replacement)

        # A short budget discards queued work after an executing transaction
        # finishes; it never interrupts that transaction or extends per lease.
        short, short_port, short_ops = spawn('gateway', drain_seconds=1)
        eventually(lambda: health(short_ops)[0] == 200, 'Short-budget gateway ready')
        short_owner = connect(short_port)
        short_player = prefix + '-short'
        assert short_owner.ask('login', player=short_player, token=os.environ['CHWELL_DEMO_TOKEN'])['ok']
        locker = lock_document(short_player)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            mutation = pool.submit(short_owner.ask, 'advance')
            fast(lambda: metric(game_ops, 'chwell_inflight_requests') == 1 and metric(short_ops, 'chwell_inflight_requests') == 1,
                       'Short-budget transaction executing')
            waiting = connect(short_port)
            waiting.socket.sendall(struct.pack('!HH', 1, len(payload)) + payload)
            fast(lambda: metric(short_ops, 'chwell_queued_inputs') >= 1, 'Short-budget input accepted')
            short.send_signal(signal.SIGUSR1)
            fast(lambda: health(short_ops)[1]['draining'], 'Short-budget drain admitted')
            time.sleep(1.05)
            unlock(locker)
            assert mutation.result(timeout=10)['ok']
        eventually(lambda: health(short_ops)[1]['drained'], 'Deadline drain completed')
        assert metric(short_ops, 'chwell_drain_dropped_inputs_total') >= 1
        assert metric(short_ops, 'chwell_queued_inputs') == 0
        assert metric(short_ops, 'chwell_inflight_requests') == 0
        raw = bytes.fromhex(sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{short_player}'")).decode()
        assert 'gold' in raw and '10' in raw, raw
        stop(short)
        game.send_signal(signal.SIGUSR1)
        eventually(lambda: health(game_ops)[1]['drained'], 'Game withdrawn and drained')
        with urllib.request.urlopen(args.endpoint + '/v1/health/service/persistent-game?passing=true', timeout=3) as reply:
            assert not json.load(reply), 'Draining Game remained discoverable'
        assert health(game_ops)[0] == 503 and health(game_ops, False)[0] == 200
        stop(game)
        print('PASS: independent probes, dependency recovery, bounded HTTP peers, metrics, accepted-work drain, lease release and replacement')
    finally:
        for locker in lockers:
            if locker.poll() is None:
                # EOF closes MySQL's transaction and releases its locks.
                locker.stdin.close()
                try:
                    locker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    locker.kill(); locker.wait(timeout=5)
        for peer in slow_peers:
            peer.close()
        for client in clients:
            client.close()
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
        for log in logs:
            log.seek(0)
            output = log.read()
            print(output[-8000:])
            log.close()
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output, output[-8000:]


if __name__ == '__main__':
    main()
