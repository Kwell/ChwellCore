"""Reference JSON client: one connection, ordered requests, no mutation retry."""
import argparse
import json
import os
import socket
import struct
import importlib.util
import pathlib

_spec = importlib.util.spec_from_file_location('chwell_sync_wire', pathlib.Path(__file__).with_name('sync_wire.py'))
sync_wire = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sync_wire)


class Client:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=12)
        self.player = None
        self.stream = None
        self.replicas = {}

    def receive(self, size):
        chunks = bytearray()
        while len(chunks) < size:
            block = self.socket.recv(size - len(chunks))
            if not block:
                raise ConnectionError('Connection closed; operation outcome may be unknown')
            chunks.extend(block)
        return bytes(chunks)

    def ask(self, action, **fields):
        fields.setdefault('sync_version', 1)
        if action == 'login':
            fields.setdefault('schema_version', sync_wire.CLUSTER_SCHEMA['version'])
        payload = json.dumps(dict(action=action, **fields)).encode()
        if len(payload) > 8192:
            raise ValueError('Reference request is too large')
        self.socket.sendall(struct.pack('!HH', 1, len(payload)) + payload)
        command, length = struct.unpack('!HH', self.receive(4))
        if command != 1:
            raise ValueError('Unexpected response command')
        if length > 8192:
            self.close()
            raise ValueError('Reference response is too large')
        response = json.loads(self.receive(length))
        if response.get('ok') and 'packet' in response:
            envelope = response['packet']
            if set(envelope) != {'encoding', 'data'} or envelope['encoding'] != 'chwell-sync-v1-hex':
                raise sync_wire.SyncError('Unsupported sync encoding')
            hex_data = envelope['data']
            if not isinstance(hex_data, str) or len(hex_data) % 2 or any(c not in '0123456789abcdef' for c in hex_data):
                raise sync_wire.SyncError('Invalid canonical hex payload')
            wire_bytes = bytes.fromhex(hex_data)
            packet = sync_wire.decode(wire_bytes, max_bytes=3072)
            if action == 'login':
                pending = sync_wire.Replica(sync_wire.CLUSTER_SCHEMA, fields['player'], True)
                pending.reset_stream(packet['stream'])
                if not packet['snapshot'] or pending.apply(wire_bytes) != 'applied':
                    raise sync_wire.SyncError('Login requires a complete snapshot')
                self.player, self.stream = fields['player'], packet['stream']
                self.replicas = {self.player: pending}
            else:
                entity = fields['target'] if action == 'observe' else self.player
                replica = self.replicas.get(entity)
                if replica is None:
                    replica = sync_wire.Replica(sync_wire.CLUSTER_SCHEMA, entity, entity == self.player)
                    replica.reset_stream(self.stream)
                status = replica.apply(wire_bytes)
                if status != 'applied':
                    raise sync_wire.SyncError('Unexpected stale response')
                self.replicas[entity] = replica
            # Preserve the example's convenient decoded Python view. The server
            # sends only binary wire data, not duplicated JSON numeric values.
            packet.pop('types')
            packet['sequence'] = str(packet['sequence'])
            packet['base_sequence'] = str(packet['base_sequence'])
            response['packet'] = packet
        if (action == 'logout' and response.get('ok')) or response.get('error') in (
                'login_required', 'session_lost', 'storage_unavailable', 'outcome_unknown', 'discovery_unavailable'):
            self.player = self.stream = None
            self.replicas = {}
        return response

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9100)
    parser.add_argument('--player', default='alice')
    parser.add_argument('--advance', action='store_true')
    args = parser.parse_args()
    with Client(args.host, args.port) as client:
        response = client.ask('login', player=args.player, token=os.environ.get('CHWELL_DEMO_TOKEN', ''))
        print(json.dumps(response, ensure_ascii=False))
        if not response.get('ok'):
            raise SystemExit(1)
        if args.advance:
            response = client.ask('advance')
            print(json.dumps(response, ensure_ascii=False))
            if not response.get('ok'):
                raise SystemExit(1)


if __name__ == '__main__':
    main()
