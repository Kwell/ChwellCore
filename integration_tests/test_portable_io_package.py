"""Install and relocate the portable root build, then consume only Chwell::io."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(command, success=True):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True,
                            encoding='utf-8', errors='replace', timeout=120)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success:
        raise RuntimeError(f'{command}\n{output}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='chwell-io-package-') as temporary:
        root = pathlib.Path(temporary)
        original, prefix = root / 'original prefix', root / 'relocated prefix'
        run([args.cmake, '--install', args.build.resolve(), '--prefix', original, '--config', args.config])
        original.rename(prefix)
        if (prefix / 'share/ChwellCore/examples').exists() or (prefix / 'share/ChwellCore/tools').exists():
            raise RuntimeError('Portable package installed application-only examples/tools')
        source = root / 'independent consumer'
        source.mkdir()
        shutil.copyfile(pathlib.Path(__file__).resolve().parents[1] / 'tests/io/main.cpp', source / 'main.cpp')
        project = source / 'CMakeLists.txt'
        project.write_text('''cmake_minimum_required(VERSION 3.18)
project(PortableIoConsumer LANGUAGES CXX)
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS io)
if(ChwellCore_WITH_CORE OR ChwellCore_core_FOUND OR TARGET Chwell::core)
    message(FATAL_ERROR "Unexpected application core")
endif()
if(ChwellCore_WITH_YAML OR ChwellCore_WITH_HTTP_CLIENT OR TARGET CURL::libcurl)
    message(FATAL_ERROR "Unexpected optional dependency")
endif()
add_executable(io_consumer main.cpp)
target_link_libraries(io_consumer PRIVATE Chwell::io)
''', encoding='utf-8')
        build = root / 'consumer build'
        configure = [args.cmake, '-S', source, '-B', build, f'-DCMAKE_PREFIX_PATH={prefix}',
                     '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
        run(configure)
        run([args.cmake, '--build', build, '--config', args.config, '--parallel', '2'])
        candidates = [build / args.config / 'io_consumer.exe', build / 'io_consumer']
        executable = next((path for path in candidates if path.exists()), None)
        if not executable:
            raise RuntimeError('Consumer executable missing')
        output = run([executable])
        if 'PASS:' not in output:
            raise RuntimeError(output)
        print(output.strip())
        project.write_text(project.read_text().replace('COMPONENTS io)', 'COMPONENTS core)'), encoding='utf-8')
        failure = run(configure, success=False)
        if 'ChwellCore' not in failure:
            raise RuntimeError('Unavailable core component failed for an unrelated reason')
        print('PASS: portable package relocation, independent IO consumer and unavailable core rejection')


if __name__ == '__main__':
    main()
