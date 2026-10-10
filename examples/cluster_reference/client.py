"""Reference JSON client: one connection, ordered requests, no mutation retry."""
import argparse
import json
import os
import socket
import struct


class Client:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=12)

    def receive(self, size):
        chunks = bytearray()
        while len(chunks) < size:
            block = self.socket.recv(size - len(chunks))
            if not block:
                raise ConnectionError('Connection closed; operation outcome may be unknown')
            chunks.extend(block)
        return bytes(chunks)

    def ask(self, action, **fields):
        payload = json.dumps(dict(action=action, **fields)).encode()
        if len(payload) > 8192:
            raise ValueError('Reference request is too large')
        self.socket.sendall(struct.pack('!HH', 1, len(payload)) + payload)
        command, length = struct.unpack('!HH', self.receive(4))
        if command != 1:
            raise ValueError('Unexpected response command')
        return json.loads(self.receive(length))

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
