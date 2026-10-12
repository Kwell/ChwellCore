"""Drop a real MySQL COMMIT OK packet after migration; verify Unknown recovery.

Only runs against the isolated CI database. The protocol proxy disables TLS in
its greeting to inspect COM_QUERY; it is a fault-test helper, not a deployable service.
"""
import os
import socket
import subprocess
import sys
import threading
import time


def exact(stream, size):
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.recv(size - len(chunks))
        if not chunk:
            raise EOFError()
        chunks.extend(chunk)
    return bytes(chunks)


def packet(stream):
    header = exact(stream, 4)
    return header, exact(stream, int.from_bytes(header[:3], 'little'))


def main():
    expire_work = len(sys.argv) > 2 and sys.argv[2] == '--expired-work'
    upstream = (os.environ['CHWELL_DB_HOST'], int(os.environ['CHWELL_DB_PORT']))
    dropped = threading.Event()
    stopping = threading.Event()
    sockets = []
    gate = threading.Lock()
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(0.2)

    def connection(client):
        server = socket.create_connection(upstream, timeout=15)
        client.settimeout(60)
        server.settimeout(60)
        with gate:
            sockets.extend((client, server))
        state = {'armed': False, 'commit': False, 'update': False}

        def relay(source, target, from_server):
            greeting = from_server
            try:
                while not stopping.is_set():
                    header, payload = packet(source)
                    if greeting:
                        greeting = False
                        if payload and payload[0] == 10:
                            lower = payload.index(b'\0', 1) + 1 + 4 + 8 + 1
                            caps = int.from_bytes(payload[lower:lower + 2], 'little') & ~0x0800
                            payload = payload[:lower] + caps.to_bytes(2, 'little') + payload[lower + 2:]
                    if not from_server and payload[:1] == b'\x03':
                        query = payload[1:].strip().upper()
                        state['commit'] = query == b'COMMIT'
                        state['update'] = query.startswith(b'UPDATE `MIGRATION_CONTRACT_KV` SET V=')
                        if state['update']:
                            state['armed'] = True
                    if from_server and expire_work and state['update'] and payload[:1] == b'\x00':
                        with gate:
                            delay = not dropped.is_set()
                            if delay:
                                dropped.set()
                        if delay:
                            time.sleep(1.4)  # UPDATE ran; lease expires before its response is delivered.
                    if from_server and not expire_work and state['armed'] and state['commit'] and payload[:1] == b'\x00':
                        with gate:
                            if not dropped.is_set():
                                dropped.set()  # Server has committed; discard the OK response.
                                return
                    target.sendall(header + payload)
            except (OSError, EOFError, ValueError):
                pass
            finally:
                for stream in (client, server):
                    try:
                        stream.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    stream.close()

        threading.Thread(target=relay, args=(client, server, False), daemon=True).start()
        threading.Thread(target=relay, args=(server, client, True), daemon=True).start()

    def accept():
        while not stopping.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=connection, args=(client,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    environment = dict(os.environ, CHWELL_DB_HOST='127.0.0.1',
                       CHWELL_DB_PORT=str(listener.getsockname()[1]))
    try:
        result = subprocess.run([sys.argv[1], '--expired-work' if expire_work else '--lost-commit'],
                                env=environment, timeout=150)
        if result.returncode != 0:
            raise RuntimeError('Migration recovery contract failed')
        if not dropped.is_set():
            raise RuntimeError('Required migration response was not intercepted')
    finally:
        stopping.set()
        listener.close()
        with gate:
            for stream in sockets:
                stream.close()


if __name__ == '__main__':
    main()
