"""Smoke-test the documented Compose deployment and persistent volume recovery."""
import os
import json
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

    def healthy():
        output = subprocess.check_output(command + ['ps', '--format', 'json'], cwd=root, text=True, timeout=15)
        # Compose v2 emits one JSON object per line.
        entries = json.loads(output) if output.lstrip().startswith('[') else [json.loads(line) for line in output.splitlines() if line.strip()]
        roles = {entry['Service']: entry for entry in entries if entry['Service'] in
                 ('game-one', 'game-two', 'gateway', 'gateway-two')}
        return len(roles) == 4 and all(entry.get('Health') == 'healthy' for entry in roles.values())

    try:
        compose('config', '--quiet')
        compose('up', '-d', '--build', timeout=600)
        eventually(healthy, 'All reference role readiness healthchecks', timeout=60)
        # Check the optional monitoring configuration with the real Prometheus parser.
        compose('run', '--rm', '--no-deps', '--entrypoint', '/bin/promtool', 'prometheus',
                'check', 'config', '/etc/prometheus/prometheus.yml')
        with eventually(ready_client, 'Compose gateway and game routes ready', timeout=60) as client:
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields']['20'] == 0, result
            result = client.ask('advance')
            assert result['ok'] and result['packet']['fields'] == {'10': 2, '20': 10}, result
            with eventually(lambda: ready_client(9101), 'Second Compose gateway and routes ready', timeout=60) as other:
                assert other.ask('login', player='compose-player', token='local-demo-token')['error'] == 'already_logged_in'
        # Planned gateway replacement withdraws readiness and finishes its worker
        # before SIGTERM; the other gateway continues to serve new connections.
        compose('kill', '-s', 'SIGUSR1', 'gateway')
        eventually(lambda: subprocess.run(command + ['exec', '-T', 'gateway', 'cluster_reference',
                   'probe', '9300', 'ready'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=5).returncode == 1, 'Draining gateway unready')
        with eventually(lambda: ready_client(9101), 'Other gateway remains ready') as other:
            assert other.ask('status')['ok']
        compose('restart', 'gateway')
        eventually(healthy, 'Gateway replacement ready', timeout=60)
        compose('restart', 'game-one', 'game-two', 'gateway', 'gateway-two')
        eventually(healthy, 'Restarted roles ready', timeout=60)
        with eventually(ready_client, 'Compose gateway and routes recovered', timeout=60) as client:
            result = client.ask('login', player='compose-player', token='local-demo-token')
            assert result['ok'] and result['packet']['fields'] == {'1': 'compose-player', '10': 2, '20': 10}, result
        # Stop writers and preserve BOTH player data and epoch rows in a logical
        # backup, then restore to an isolated database to verify the restore path.
        compose('stop', 'gateway', 'gateway-two', 'game-one', 'game-two')
        def database(*args, stdin=None):
            return subprocess.run(command + ['exec', '-T', '-e', 'MYSQL_PWD=local-root-password',
                                  'mysql'] + list(args), input=stdin, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, check=True, cwd=root, timeout=60).stdout
        dump = database('mysqldump', '--protocol=TCP', '-h127.0.0.1', '-uroot',
                        '--single-transaction', '--no-tablespaces', '--set-gtid-purged=OFF', 'chwell')
        database('mysql', '--protocol=TCP', '-h127.0.0.1', '-uroot', '-e', 'CREATE DATABASE restore_check')
        database('mysql', '--protocol=TCP', '-h127.0.0.1', '-uroot', 'restore_check', stdin=dump)
        verified = database('mysql', '--protocol=TCP', '-h127.0.0.1', '-uroot', '-N', '-B', '-e',
                            "SELECT COUNT(*) FROM chwell.cluster_kv a JOIN restore_check.cluster_kv b ON a.k=b.k "
                            "WHERE a.k='cluster_players:compose-player' AND BINARY a.v=BINARY b.v; "
                            "SELECT COUNT(*) FROM chwell.chwell_session_leases a JOIN restore_check.chwell_session_leases b "
                            "ON a.player=b.player WHERE a.player='compose-player' AND a.epoch=b.epoch; "
                            "SELECT COUNT(*) FROM chwell.chwell_request_results a JOIN restore_check.chwell_request_results b "
                            "ON a.document_table=b.document_table AND a.player=b.player AND a.request_id=b.request_id "
                            "WHERE a.player='compose-player' AND a.command=b.command AND a.result=b.result AND a.format=b.format;")
        assert verified.splitlines() == [b'1', b'1', b'1'], verified
        database('mysql', '--protocol=TCP', '-h127.0.0.1', '-uroot', '-e', 'DROP DATABASE restore_check')
        print('PASS: role healthchecks, monitoring config, gateway drain/restart, volume recovery and isolated logical restore')
    finally:
        compose('logs', '--no-color', '--tail', '100')
        compose('down', '-v', '--remove-orphans')


if __name__ == '__main__':
    main()
