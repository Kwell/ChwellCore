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

    def ready_client(port=9100):
        # Docker's published port may accept TCP while the restarted gateway
        # is still starting. Probe the application and discard failed sockets;
        # retrying requests forever on a closed connection cannot recover.
        client = Client('127.0.0.1', port)
        try:
            if set(client.ask('status')['nodes']) == {'game-one', 'game-two'}:
                return client
        except BaseException:
            client.close()
            raise
        client.close()
        return None

    try:
        compose('config', '--quiet')
        compose('up', '-d', '--build', timeout=600)
        with eventually(ready_client, 'Compose gateway and game routes ready', timeout=60) as client:
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields']['20'] == 0, result
            result = client.ask('advance')
            assert result['ok'] and result['packet']['fields'] == {'10': 2, '20': 10}, result
            with eventually(lambda: ready_client(9101), 'Second Compose gateway and routes ready', timeout=60) as other:
                assert other.ask('login', player='compose-player', token='local-demo-token')['error'] == 'already_logged_in'
        compose('restart', 'game-one', 'game-two', 'gateway', 'gateway-two')
        with eventually(ready_client, 'Compose gateway and routes recovered', timeout=60) as client:
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields'] == {'1': 'compose-player', '10': 2, '20': 10}, result
        print('PASS: documented Compose build, numeric endpoints, real MySQL volume and restart recovery')
    finally:
        compose('logs', '--no-color', '--tail', '100')
        compose('down', '-v', '--remove-orphans')


if __name__ == '__main__':
    main()
