"""Smoke-test the documented Compose deployment and persistent volume recovery."""
import os
import pathlib
import subprocess

from test_persistent_cluster import Client
from test_consul_cluster import eventually


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    command = ['docker', 'compose', '-p', 'chwell-reference-ci', '-f',
               str(root / 'examples/cluster_reference/compose.yml')]

    def compose(*args, timeout=120):
        subprocess.run(command + list(args), check=True, cwd=root, timeout=timeout)

    def connect():
        return Client('127.0.0.1', 9100)

    try:
        compose('config', '--quiet')
        compose('up', '-d', '--build', timeout=600)
        with eventually(connect, 'Compose gateway listening', timeout=60) as client:
            eventually(lambda: set(client.ask('status')['nodes']) == {'game-one', 'game-two'}, 'Compose game routes')
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields']['20'] == 0, result
            result = client.ask('advance')
            assert result['ok'] and result['packet']['fields'] == {'10': 2, '20': 10}, result
        compose('restart', 'game-one', 'game-two', 'gateway')
        with eventually(connect, 'Compose gateway restarted', timeout=60) as client:
            eventually(lambda: set(client.ask('status')['nodes']) == {'game-one', 'game-two'}, 'Compose routes recovered')
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields'] == {'1': 'compose-player', '10': 2, '20': 10}, result
        print('PASS: documented Compose build, numeric endpoints, real MySQL volume and restart recovery')
    finally:
        compose('logs', '--no-color', '--tail', '100')
        compose('down', '-v', '--remove-orphans')


if __name__ == '__main__':
    main()
