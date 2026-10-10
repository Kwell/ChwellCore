"""Real TCP gateway / two games / Consul / MySQL fault and persistence contract."""
import argparse
import hashlib
import importlib.util
import os
import pathlib
import socket
import struct
import subprocess
import tempfile

from test_consul_cluster import eventually, free_port

client_path = pathlib.Path(__file__).resolve().parents[1] / 'examples/cluster_reference/client.py'
spec = importlib.util.spec_from_file_location('cluster_client', client_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
Client = module.Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8500')
    parser.add_argument('--mysql-container', required=True)
    parser.add_argument('--consul-container', required=True)
    args = parser.parse_args()
    token = os.environ['CHWELL_DEMO_TOKEN']
    prefix = 'persistent-' + str(os.getpid())
    nodes = {prefix + '-one', prefix + '-two'}
    processes, logs, clients = [], [], []
    gateway_port = free_port()

    def spawn(role, identity=None, generation='v1', environment=None):
        command = [args.executable, role, args.endpoint]
        command += [identity, str(free_port()), generation] if identity else [str(gateway_port)]
        log = tempfile.TemporaryFile(mode='w+t')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment)
        processes.append(process)
        return process

    def stop(process, abrupt=False):
        if process.poll() is None:
            process.kill() if abrupt else process.terminate()
            process.wait(timeout=15)
            if not abrupt:
                assert process.returncode == 0, process.returncode

    def connect():
        client = Client('127.0.0.1', gateway_port)
        clients.append(client)
        return client

    def docker(*command):
        return subprocess.check_output(['docker', *command], text=True, timeout=30).strip()

    def stored(player):
        # Query the external database independently of the framework to prove commit.
        # Demo passwords are passed via the inherited MYSQL_PWD variable, not argv.
        return docker('exec', '-e', 'MYSQL_PWD', args.mysql_container, 'mysql', '-uchwell',
                      '-N', '-B', 'chwell', '-e',
                      f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{player}'")

    first = second = gateway = None
    try:
        # Wrong credentials must fail startup, never fall back to in-memory success.
        bad_env = dict(os.environ, CHWELL_DB_PASSWORD='incorrect-reference-password')
        bad = spawn('game', prefix + '-bad', environment=bad_env)
        assert bad.wait(timeout=15) != 0
        first_id, second_id = sorted(nodes)
        first = spawn('game', first_id)
        second = spawn('game', second_id)
        gateway = spawn('gateway')
        status = eventually(connect, 'TCP gateway listening')
        eventually(lambda: set(status.ask('status')['nodes']) == nodes, 'Two database-backed game nodes')
        assert status.ask('advance')['error'] == 'login_required'
        assert status.ask('login', player='alice', token='wrong')['error'] == 'unauthorized'
        assert status.ask('login', player='../bad', token=token)['error'] == 'bad_player'
        status.socket.sendall(struct.pack('!HH', 1, 1) + b'{')
        command, length = struct.unpack('!HH', status.receive(4))
        assert command == 1 and b'bad_request' in status.receive(length)

        # Spread keys deterministically rather than depending on incidental hash balance.
        reached, players, owners = set(), {}, {}
        for sample in range(256):
            player = hashlib.sha256(f'reference-{sample}'.encode()).hexdigest()
            client = connect()
            result = client.ask('login', player=player, token=token)
            assert result['ok'], result
            node = result['node']
            fields = result['packet']['fields']
            assert fields == {'1': player, '10': 1, '20': 0}, result
            if node in reached:
                assert client.ask('logout')['ok']
                client.close()
                continue
            reached.add(node)
            players[node], owners[node] = player, client
            if reached == nodes:
                break
        assert reached == nodes, reached
        player, owner = players[first_id], owners[first_id]
        observer = owners[second_id]
        duplicate = connect()
        assert duplicate.ask('login', player=player, token=token)['error'] == 'already_logged_in'

        result = owner.ask('advance', player=players[second_id], viewer=players[second_id])
        assert result['ok'] and result['node'] == first_id and not result['packet']['snapshot'], result
        assert result['packet']['fields'] == {'10': 2, '20': 10}, result
        result = observer.ask('observe', target=player, viewer=player)
        assert result['ok'] and result['packet']['fields'] == {'1': player, '10': 2}, result
        assert observer.ask('get')['packet']['fields']['10'] == 1
        persisted = stored(player)
        raw = bytes.fromhex(persisted).decode()
        assert 'server-only' in raw and player in raw, raw

        # Game disappearance invalidates a bound session, never silently moves it.
        stop(first, abrupt=True)
        eventually(lambda: status.ask('status')['nodes'] == [second_id], 'TTL withdrawal')
        assert owner.ask('advance')['error'] == 'login_required'
        assert stored(player) == persisted, 'Mutation was replayed to the surviving node'
        first = spawn('game', first_id, 'v2')
        eventually(lambda: set(status.ask('status')['nodes']) == nodes, 'Replacement registered')
        result = owner.ask('login', player=player, token=token)
        assert result['ok'] and result['node'] == first_id and result['generation'] == 'v2', result
        assert result['packet']['fields'] == {'1': player, '10': 2, '20': 10}, result

        # Disconnect releases the gateway-local binding and allows fresh login.
        owner.close()
        def reconnect():
            result = duplicate.ask('login', player=player, token=token)
            return result if result['ok'] else None
        eventually(reconnect, 'Disconnected session cleaned up')

        # Gateway restart loses local sessions; data survives in the database.
        stop(gateway)
        gateway = spawn('gateway')
        status = eventually(connect, 'Gateway restarted')
        eventually(lambda: set(status.ask('status')['nodes']) == nodes, 'Routes restored')
        assert status.ask('get')['error'] == 'login_required'
        result = status.ask('login', player=player, token=token)
        assert result['ok'] and result['packet']['fields']['20'] == 10, result

        # Registry failure withdraws routes and sessions. Restore requires login.
        docker('stop', '-t', '1', args.consul_container)
        eventually(lambda: not status.ask('status')['ok'], 'Registry failure closed routes')
        assert status.ask('advance')['error'] == 'discovery_unavailable'
        docker('start', args.consul_container)
        eventually(lambda: set(status.ask('status')['nodes']) == nodes, 'Registry restart recovery', timeout=45)
        assert status.ask('get')['error'] == 'login_required'
        assert status.ask('login', player=player, token=token)['ok']

        # Real DB outage reports failure, never acknowledges an uncommitted change.
        docker('stop', '-t', '1', args.mysql_container)
        result = status.ask('advance')
        assert not result['ok'] and result['error'] in ('storage_unavailable', 'outcome_unknown'), result
        fresh = connect()
        result = fresh.ask('login', player='outage-player', token=token)
        assert not result['ok'], result
        docker('start', args.mysql_container)
        def mysql_ready():
            result = subprocess.run(['docker', 'exec', '-e', 'MYSQL_PWD', args.mysql_container,
                                     'mysql', '-uchwell', '-e', 'SELECT 1', 'chwell'],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            return result.returncode == 0
        eventually(mysql_ready, 'MySQL restarted', timeout=60)
        assert stored(player) == persisted, 'Failed request changed committed player data'
        assert not stored('outage-player'), 'Outage was treated as a missing player'
        eventually(lambda: status.ask('get').get('ok'), 'Game storage connection recovered')
        assert status.ask('get')['packet']['fields']['20'] == 10
        assert status.ask('advance')['packet']['fields'] == {'10': 3, '20': 20}
        assert stored(player) != persisted

        # Empty/wrong internal credential must also be rejected by the Game.
        import json
        import urllib.request
        with urllib.request.urlopen(args.endpoint + '/v1/catalog/service/persistent-game', timeout=5) as response:
            catalog = json.load(response)
        backend = next(entry for entry in catalog if entry['ServiceID'] == first_id)
        with socket.create_connection((backend['ServiceAddress'], backend['ServicePort']), timeout=10) as direct:
            payload = b'\x00\x00\x00\x01' + b'{"action":"get","player":"alice"}'
            direct.sendall(struct.pack('!HH', 1, len(payload)) + payload)
            def receive(size):
                result = b''
                while len(result) < size:
                    block = direct.recv(size - len(result))
                    assert block
                    result += block
                return result
            _, length = struct.unpack('!HH', receive(4))
            body = receive(length)
            assert body[:4] == payload[:4] and json.loads(body[4:])['error'] == 'unauthorized'

        assert status.ask('logout')['ok']
        assert status.ask('get')['error'] == 'login_required'
        stop(gateway); stop(first); stop(second)
        print('PASS: TCP client/gateway/two games, MySQL commit, owner/public filtering, '
              'TTL and process recovery, registry/DB outages, authentication and graceful shutdown')
    finally:
        for client in clients:
            client.close()
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for log in logs:
            log.seek(0)
            output = log.read()
            print(output[-8000:])
            log.close()
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output, output[-8000:]


if __name__ == '__main__':
    main()
