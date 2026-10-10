"""Install, relocate, and consume without the framework source/build include paths.

Also checks real TCP echo/shutdown and catalog dependency regeneration. POSIX only.
"""
import argparse
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import tempfile
import time


def run(command, success=True):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=180)
    if (result.returncode == 0) != success:
        raise RuntimeError(f'{command}\n{result.stdout}\n{result.stderr}')
    return result.stdout + result.stderr


def changed(path, text):
    previous = path.stat().st_mtime
    path.write_text(text, encoding='utf-8')
    # Make dependencies newer even on filesystems with coarse timestamp resolution.
    timestamp = max(time.time(), previous) + 2
    os.utime(path, (timestamp, timestamp))


def verify_server(executable, configuration):
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    configuration.write_text(f'listen_port={port}\nworker_threads=2\n'
                             'component.ReferenceGame.enabled=true\n', encoding='utf-8')
    process = subprocess.Popen([str(executable), '--serve', str(configuration)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 15
        while True:
            if process.poll() is not None: raise RuntimeError('Server exited before readiness')
            try:
                client = socket.create_connection(('127.0.0.1', port), timeout=1)
                break
            except OSError:
                if time.monotonic() >= deadline: raise RuntimeError('TCP readiness timeout')
                time.sleep(0.05)
        with client:
            payload = b'installed-reference\x00echo\n'
            client.sendall(payload)
            received = b''
            while len(received) < len(payload):
                block = client.recv(len(payload) - len(received))
                if not block: raise RuntimeError('Connection closed during echo')
                received += block
            if received != payload: raise RuntimeError('Echo changed bytes')
        process.send_signal(signal.SIGTERM)
        output, _ = process.communicate(timeout=15)
        if process.returncode != 0 or 'PASS:' not in output:
            raise RuntimeError('Graceful shutdown failed\n' + output)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='chwell-package-') as temporary:
        root = pathlib.Path(temporary)
        prefix = root / 'original prefix'
        run([args.cmake, '--install', build, '--prefix', prefix, '--config', args.config])
        relocated = root / 'relocated prefix'
        prefix.rename(relocated)
        for path in relocated.rglob('ChwellCore*.cmake'):
            if str(build) in path.read_text(encoding='utf-8') or str(prefix) in path.read_text(encoding='utf-8'):
                raise RuntimeError(f'Non-relocatable package: {path}')
        source = root / 'independent source'
        shutil.copytree(relocated / 'share/ChwellCore/examples/reference_service', source)
        shutil.copyfile(pathlib.Path(__file__).with_name('package_probe.cpp'), source / 'package_probe.cpp')
        with (source / 'CMakeLists.txt').open('a', encoding='utf-8') as cmake:
            cmake.write('''
add_executable(package_probe package_probe.cpp)
target_link_libraries(package_probe PRIVATE Chwell::core)
if(TARGET Chwell::consul)
    target_link_libraries(package_probe PRIVATE Chwell::consul)
    target_compile_definitions(package_probe PRIVATE PACKAGE_HAS_CONSUL)
endif()
if(TARGET Chwell::game_proto)
    target_link_libraries(package_probe PRIVATE Chwell::game_proto)
    target_compile_definitions(package_probe PRIVATE PACKAGE_HAS_PROTOBUF)
endif()
if(ChwellCore_WITH_MONGODB)
    target_compile_definitions(package_probe PRIVATE PACKAGE_HAS_MONGODB)
endif()
''')
        consumer = root / 'consumer build'
        run([args.cmake, '-S', source, '-B', consumer, f'-DCMAKE_PREFIX_PATH={relocated}',
             f'-DCMAKE_BUILD_TYPE={args.config}', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
             '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF'])
        compile_command = [args.cmake, '--build', consumer, '--config', args.config, '--parallel', '2']
        run(compile_command)
        run([consumer / 'package_probe'])
        executable = consumer / 'reference_service'
        output = run([executable, '--smoke'])
        if 'PASS:' not in output or 'power=10' not in output: raise RuntimeError(output)
        verify_server(executable, root / 'runtime.conf')

        # Existing target CSV must trigger validation, leaving both generated headers intact.
        content = source / 'content'
        headers = {path: path.read_bytes() for path in (consumer / 'generated').glob('*.h')}
        changed(content / 'items.csv', 'id,power\ndagger,11\n')
        failure = run(compile_command, success=False)
        if 'unresolved reference' not in failure: raise RuntimeError(failure)
        if any(path.read_bytes() != value for path, value in headers.items()):
            raise RuntimeError('Failed catalog validation replaced an artifact')
        changed(content / 'items.csv', 'id,power\nsword,11\nstaff,8\n')
        run(compile_command)
        if 'power=11' not in run([executable, '--smoke']): raise RuntimeError('Stale generated content')

        # Editing the manifest must reconfigure and track the newly added dependency.
        extra = json.loads((content / 'item.schema.json').read_text())
        extra['name'] = 'ExtraItem'; extra['table'] = 'extras'
        (content / 'extra.schema.json').write_text(json.dumps(extra))
        (content / 'extra.csv').write_text('id,power\nx,1\n')
        catalog = json.loads((content / 'catalog.json').read_text())
        catalog['tables'].append({'schema': 'extra.schema.json', 'csv': 'extra.csv'})
        changed(content / 'catalog.json', json.dumps(catalog))
        run(compile_command)
        changed(content / 'extra.csv', 'id,power\nx,9999\n')
        failure = run(compile_command, success=False)
        if 'extra.csv:2:2' not in failure: raise RuntimeError('New dependency not tracked\n' + failure)
        print('PASS: relocated package, independent consumer, TCP shutdown, atomic catalog and rebuild dependencies')


if __name__ == '__main__':
    main()
