"""Exercise the native client against real local HTTP and TLS sockets."""
import gzip
import http.server
import os
import pathlib
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.lock = threading.Lock()
        self.mutations = 0
        self.redirect_hits = 0
        self.cancelled_hits = 0


class StalledTls(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        try:
            self.request.recv(8192)
            time.sleep(3)  # Accept TCP but never complete TLS.
        except OSError:
            pass


class StalledServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, ssl.SSLError):
            pass  # The contract deliberately cancels or rejects several responses.

    def reply(self, status, body, headers=()):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        self.send_response(200)
        self.send_header('Content-Length', '1000000')
        self.end_headers()

    def do_GET(self):
        if self.path == '/binary':
            self.reply(200, b'hello\0world', [('X-Repeat', 'one'), ('X-Repeat', 'two'), ('X-Empty', '')])
        elif self.path == '/echo':
            body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
            headers = [('X-Received-Empty', 'yes')] if self.headers.get('X-Empty') == '' else []
            self.reply(200, self.command.encode() + b':' + body, headers)
        elif self.path == '/chunked':
            self.wfile.write(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Checksum\r\n\r\n'
                             b'3\r\na\0b\r\n0\r\nX-Checksum: complete\r\n\r\n')
        elif self.path == '/interim':
            self.wfile.write(b'HTTP/1.1 103 Early Hints\r\nX-Interim: ' + b'x' * 80 + b'\r\n\r\n'
                             b'HTTP/1.1 200 OK\r\nX-Final: yes\r\nContent-Length: 2\r\n\r\nok')
        elif self.path == '/redirect':
            self.reply(302, b'', [('Location', '/redirect-target')])
        elif self.path == '/redirect-target':
            with self.server.lock:
                self.server.redirect_hits += 1
            self.reply(200, b'followed')
        elif self.path == '/pre-cancelled':
            with self.server.lock:
                self.server.cancelled_hits += 1
            self.reply(200, b'unexpected')
        elif self.path == '/unavailable':
            self.reply(503, b'unavailable')
        elif self.path == '/compressed':
            body = b'x' * 4096
            headers = []
            if 'gzip' in self.headers.get('Accept-Encoding', ''):
                body = gzip.compress(body)
                headers = [('Content-Encoding', 'gzip')]
            self.reply(200, body, headers)
        elif self.path == '/large-header':
            self.reply(200, b'', [('X-Large', 'x' * 2000)])
        elif self.path == '/slow-header':
            time.sleep(1.2)
            self.reply(200, b'late')
        elif self.path == '/slow-body':
            self.send_response(200)
            self.send_header('Content-Length', '4')
            self.end_headers()
            self.wfile.flush()
            time.sleep(1.2)
            self.wfile.write(b'late')
        elif self.path == '/drip':
            self.send_response(200)
            self.send_header('Content-Length', '100')
            self.end_headers()
            for _ in range(100):
                self.wfile.write(b'x')
                self.wfile.flush()
                time.sleep(0.04)
        elif self.path == '/no-read':
            time.sleep(1.2)
            self.close_connection = True
        elif self.path == '/mutate':
            self.rfile.read(int(self.headers.get('Content-Length', 0)))
            with self.server.lock:
                self.server.mutations += 1
            time.sleep(1.2)
            self.reply(200, b'committed')
        elif self.path == '/stats':
            with self.server.lock:
                count = self.server.mutations
            self.reply(200, str(count).encode())
        elif self.path == '/disconnect':
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
        elif self.path == '/partial':
            self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort')
            self.wfile.flush()
            self.close_connection = True
        else:
            self.reply(404, b'unknown path')

    do_POST = do_PUT = do_PATCH = do_DELETE = do_OPTIONS = do_GET


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: test_http_client.py native-contract-executable')
    openssl = shutil.which('openssl')
    if not openssl:
        git = shutil.which('git')
        candidate = (pathlib.Path(git).resolve().parent.parent / 'usr/bin/openssl.exe'
                     if git else pathlib.Path('C:/Program Files/Git/usr/bin/openssl.exe'))
        if candidate.exists():
            openssl = str(candidate)
    if not openssl:
        raise RuntimeError('OpenSSL CLI is required to generate an isolated test certificate')
    with tempfile.TemporaryDirectory(prefix='chwell-http-') as temporary:
        root = pathlib.Path(temporary)
        cert, key = root / 'ca.pem', root / 'key.pem'
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-addext', 'basicConstraints=critical,CA:TRUE', '-keyout', str(key), '-out', str(cert)],
                       check=True, capture_output=True, timeout=30)
        plain, secure = Server(), Server()
        stalled = StalledServer(('127.0.0.1', 0), StalledTls)
        servers = (plain, secure, stalled)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in servers]
        for thread in threads:
            thread.start()
        try:
            environment = os.environ.copy()
            # A bogus proxy must not divert explicit local connections.
            environment.update(http_proxy='http://127.0.0.1:1', https_proxy='http://127.0.0.1:1',
                               HTTP_PROXY='http://127.0.0.1:1', HTTPS_PROXY='http://127.0.0.1:1', NO_PROXY='')
            subprocess.run([sys.argv[1], f'http://127.0.0.1:{plain.server_port}',
                            f'https://localhost:{secure.server_port}', str(cert),
                            f'https://127.0.0.1:{stalled.server_address[1]}'],
                           check=True, env=environment, timeout=30)
            if plain.redirect_hits != 0 or plain.mutations != 1 or plain.cancelled_hits != 0:
                raise RuntimeError('Client followed a redirect, replayed a mutation or sent a pre-cancelled request')
            print('PASS: real sockets, verified TLS, no redirect or mutation replay')
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=2)


if __name__ == '__main__':
    main()
