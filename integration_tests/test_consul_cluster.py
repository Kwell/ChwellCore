"""Exercise separate game/gateway processes against a real local Consul agent.

Usage: python3 integration_tests/test_consul_cluster.py build/example_discovery_cluster
       --endpoint http://127.0.0.1:8500 --consul-container chwell-consul
The optional container argument additionally tests registry loss and restart.
"""
import argparse
import json
import os
import queue
import socket
import subprocess
import threading
import time
import urllib.request


def eventually(predicate, description, timeout=25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, ValueError, AssertionError) as error:
            last = str(error)
        time.sleep(0.25)
    raise AssertionError(f'{description}: {last}')


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class Probe:
    def __init__(self, executable, endpoint):
        self.process = subprocess.Popen([executable, 'probe', endpoint], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.output = queue.Queue()

        def read_output():
            for line in self.process.stdout:
                if line.startswith('RESULT '):
                    self.output.put(json.loads(line[len('RESULT '):]))

        self.reader = threading.Thread(target=read_output, daemon=True)
        self.reader.start()

    def ask(self, action, **fields):
        self.process.stdin.write(json.dumps(dict(action=action, **fields)) + '\n')
        self.process.stdin.flush()
        try:
            return self.output.get(timeout=6)
        except queue.Empty:
            raise AssertionError('Gateway probe did not answer within its deadline')

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        if self.process.returncode:
            raise AssertionError('Probe failed: ' + self.process.stderr.read())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8500')
    parser.add_argument('--consul-container')
    args = parser.parse_args()
    processes = []
    prefix = 'chwell-' + str(os.getpid())
    one, two = prefix + '-one', prefix + '-two'

    def spawn(identity, generation):
        process = subprocess.Popen([args.executable, 'game', args.endpoint, identity,
                                    str(free_port()), generation], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
        processes.append(process)
        return process

    probe = Probe(args.executable, args.endpoint)
    first = spawn(one, 'v1')
    second = spawn(two, 'v1')
    try:
        eventually(lambda: set(probe.ask('refresh')['nodes']) == {one, two}, 'Two independent games registered')
        reached = set()
        keys = {}
        for key in range(80):
            result = probe.ask('route', key=str(key), payload='hello')
            assert result['ok'], result
            identity, payload = result['response'].split('|', 1)
            assert payload == 'hello', result
            node = identity.split('@', 1)[0]
            reached.add(node)
            keys[node] = str(key)
            if reached == {one, two}:
                break
        assert reached == {one, two}, reached
        assert probe.ask('bind', session='alice', node=one)['ok']
        assert probe.ask('session', session='alice')['response'].startswith(one + '@v1|')
        first.kill()  # No deregistration: disappearance must come from real TTL expiry.
        first.wait(timeout=5)
        eventually(lambda: probe.ask('refresh')['nodes'] == [two], 'Dead game withdrawn after TTL expiry')
        result = probe.ask('session', session='alice')
        assert not result['ok'] and result['sessions'] == 0, result
        result = probe.ask('route', key=keys[one])
        assert result['ok'] and result['response'].startswith(two + '@v1|'), result
        replacement = spawn(one, 'v2')  # Same ID, new endpoint and process incarnation.
        eventually(lambda: set(probe.ask('refresh')['nodes']) == {one, two}, 'Game rejoined')
        result = probe.ask('route', key=keys[one])
        assert result['ok'] and result['response'].startswith(one + '@v2|'), result
        assert probe.ask('bind', session='alice', node=one)['ok']
        assert probe.ask('session', session='alice')['response'].startswith(one + '@v2|')
        if args.consul_container:
            subprocess.run(['docker', 'stop', '-t', '1', args.consul_container], check=True, stdout=subprocess.DEVNULL)
            result = probe.ask('refresh')
            assert not result['ok'] and not result['nodes'] and result['sessions'] == 0, result
            subprocess.run(['docker', 'start', args.consul_container], check=True, stdout=subprocess.DEVNULL)
            eventually(lambda: set(probe.ask('refresh')['nodes']) == {one, two}, 'Registry restart and heartbeat recovery', timeout=40)
            result = probe.ask('route', key=keys[one])
            assert result['ok'] and result['response'].startswith(one + '@v2|'), result
        second.terminate()
        second.wait(timeout=8)
        assert second.returncode == 0, second.stderr.read()
        eventually(lambda: probe.ask('refresh')['nodes'] == [one], 'Graceful deregistration')
        replacement.terminate()
        replacement.wait(timeout=8)
        assert replacement.returncode == 0, replacement.stderr.read()
        eventually(lambda: not probe.ask('refresh')['nodes'], 'Empty healthy cluster')
        print('PASS: real Consul discovery, two game processes, RPC routing, TTL loss, session invalidation, reconnect and graceful shutdown')
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        probe.close()


if __name__ == '__main__':
    main()
