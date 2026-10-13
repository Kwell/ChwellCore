"""Lose a committed client response, restart both roles, recover the durable outcome without a second mutation."""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import tempfile
import threading

from test_consul_cluster import eventually, free_port
from test_persistent_cluster import Client


def exact(stream, size):
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError()
        data.extend(chunk)
    return bytes(data)


class DropReply:
    """Ordered framing proxy; discards one successful advance reply after receiving it."""
    def __init__(self, upstream):
        self.upstream = upstream
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.stopping = threading.Event()
        self.dropped = threading.Event()
        self.lock = threading.Lock()
        self.sockets = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stopping.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.relay, args=(client,), daemon=True).start()

    def relay(self, client):
        server = None
        try:
            server = socket.create_connection(('127.0.0.1', self.upstream), timeout=12)
            client.settimeout(12)
            with self.lock:
                self.sockets.extend((client, server))
            while not self.stopping.is_set():
                header = exact(client, 4)
                _, size = struct.unpack('!HH', header)
                body = exact(client, size)
                command = json.loads(body)
                server.sendall(header + body)
                header = exact(server, 4)
                _, size = struct.unpack('!HH', header)
                body = exact(server, size)
                result = json.loads(body)
                if command['action'] == 'advance' and result.get('ok') and not self.dropped.is_set():
                    self.dropped.set()
                    return
                client.sendall(header + body)
        except (OSError, EOFError, ValueError):
            pass
        finally:
            for stream in (client, server):
                if stream:
                    stream.close()

    def close(self):
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=2)
        with self.lock:
            for stream in self.sockets:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8500')
    parser.add_argument('--mysql-container', required=True)
    args = parser.parse_args()
    executable = str(pathlib.Path(args.executable).resolve())
    prefix = 'dedup-' + str(os.getpid())
    processes, logs, clients = [], [], []
    game_port, gateway_port, other_port = free_port(), free_port(), free_port()
    token = os.environ['CHWELL_DEMO_TOKEN']

    def spawn(role, port):
        command = [executable, role, args.endpoint]
        command += [prefix + '-game', str(port), 'v1'] if role == 'game' else [str(port)]
        log = tempfile.TemporaryFile(mode='w+t')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def connect(port):
        client = Client('127.0.0.1', port)
        clients.append(client)
        return client

    def ready(port):
        client = connect(port)
        try:
            if prefix + '-game' in client.ask('status')['nodes']:
                return client
        except BaseException:
            client.close()
            raise
        client.close()
        return None

    def sql(statement):
        return subprocess.check_output(['docker', 'exec', '-e', 'MYSQL_PWD', args.mysql_container,
                                        'mysql', '--protocol=TCP', '-h127.0.0.1', '-uchwell', '-N', '-B',
                                        'chwell', '-e', statement], text=True, timeout=15).strip()

    def stop(process):
        if process.poll() is None:
            process.terminate()
            assert process.wait(timeout=25) == 0

    proxy = None
    try:
        game = spawn('game', game_port)
        gateway = spawn('gateway', gateway_port)
        other = spawn('gateway', other_port)
        eventually(lambda: ready(gateway_port), 'First gateway ready').close()
        eventually(lambda: ready(other_port), 'Second gateway ready').close()
        proxy = DropReply(gateway_port)
        owner = connect(proxy.port)
        player = prefix + '-player'
        assert owner.ask('login', player=player, token=token)['ok']
        # Fresh mutation with a persisted caller ID; the proxy sees its committed
        # response, but the client never receives even the response header.
        request_id = prefix + '-one'
        try:
            owner.ask('advance', request_id=request_id)
            raise AssertionError('Required successful response was not dropped')
        except ConnectionError:
            pass
        assert proxy.dropped.is_set() and owner.last_request == {'request_id': request_id, 'steps': 1}
        stored = sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{player}'")
        assert sql(f"SELECT COUNT(*) FROM chwell_request_results WHERE player='{player}' AND request_id='{request_id}'") == '1'
        # Restart the gateway AND Game; there is no process-local dedup cache.
        stop(gateway)
        stop(game)
        game = spawn('game', game_port)
        recovered = eventually(lambda: ready(other_port), 'Replacement Game discovered')
        result = eventually(lambda: recovered.ask('login', player=player, token=token).get('ok'), 'Lease available after response loss')
        assert result
        queried = recovered.ask('request_result', request_id=request_id)
        assert queried == {'ok': True, 'request_id': request_id, 'result': {'schema_version': 1, 'level': 2, 'gold': 10}}, queried
        retried = recovered.ask('advance', request_id=request_id)
        assert retried['request']['replayed'] and retried['request']['result'] == queried['result'], retried
        assert retried['packet']['snapshot'] and retried['packet']['fields'] == {'1': player, '10': 2, '20': 10}
        assert sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{player}'") == stored, 'Duplicate changed document'
        conflict = recovered.ask('advance', request_id=request_id, steps=2)
        assert conflict['error'] == 'request_id_conflict', conflict
        assert recovered.ask('request_result', request_id=request_id, steps=2)['error'] == 'request_id_conflict'
        assert recovered.ask('request_result', request_id=prefix + '-missing')['error'] == 'request_not_found'
        assert recovered.ask('advance', request_id='')['error'] == 'bad_request_id_or_steps'
        second = recovered.ask('advance', request_id=prefix + '-two', steps=2)
        assert not second['request']['replayed'] and second['request']['result']['gold'] == 30
        # An old outcome must not overwrite a more recent client state. Result
        # queries allocate no sequence; replay supplies a new current snapshot.
        assert recovered.ask('request_result', request_id=request_id)['result']['gold'] == 10
        replayed = recovered.ask('advance', request_id=request_id)
        assert replayed['request']['result']['gold'] == 10 and replayed['packet']['fields']['20'] == 30
        assert int(replayed['packet']['sequence']) == int(second['packet']['sequence']) + 1
        stranger = connect(other_port)
        assert stranger.ask('request_result', request_id=request_id)['error'] == 'login_required'
        assert stranger.ask('login', player=prefix + '-stranger', token=token)['ok']
        assert stranger.ask('request_result', request_id=request_id, player=player)['error'] == 'request_not_found'
        independent = stranger.ask('advance', request_id=request_id)
        assert not independent['request']['replayed'] and independent['request']['result']['gold'] == 10
        # Isolated fault injection: malformed historical JSON or an unknown
        # journal format must fail closed, never delete/re-execute the request.
        stranger_key = prefix + '-stranger'
        stranger_before = sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{stranger_key}'")
        sql(f"UPDATE chwell_request_results SET result=X'7b' WHERE player='{stranger_key}' AND request_id='{request_id}'")
        assert stranger.ask('request_result', request_id=request_id)['error'] == 'invalid_request_record'
        assert stranger.ask('advance', request_id=request_id)['error'] == 'invalid_request_record'
        sql(f"UPDATE chwell_request_results SET format=2 WHERE player='{stranger_key}' AND request_id='{request_id}'")
        assert stranger.ask('request_result', request_id=request_id)['error'] == 'invalid_request_record'
        assert stranger.ask('advance', request_id=request_id)['error'] == 'invalid_request_record'
        assert sql(f"SELECT HEX(v) FROM cluster_kv WHERE k='cluster_players:{stranger_key}'") == stranger_before
        assert recovered.ask('logout')['ok']
        assert stranger.ask('logout')['ok']
        stop(other); stop(game)
        print('PASS: committed reply loss, role restart, durable recovery, explicit duplicate, parameter conflict, current snapshots and player isolation')
    finally:
        if proxy:
            proxy.close()
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
