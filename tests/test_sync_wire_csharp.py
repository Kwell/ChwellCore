"""Compile C# synchronization against shared C++ fixtures and Python differential cases."""
import argparse
import copy
import importlib.util
import json
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_sync_wire import encode, packet, sync_wire

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tool', type=pathlib.Path, default=ROOT / 'tools/entity_schema.py')
parser.add_argument('--client-directory', type=pathlib.Path, default=ROOT / 'clients/csharp')
parser.add_argument('--require-dotnet', action='store_true')
OPTIONS, remaining = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('sync_contract_generator', OPTIONS.tool)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def corpus():
    cases = {'decode': [], 'replicas': [], 'invalid_schemas': [], 'cluster_snapshot': encode(packet()).hex()}

    def decoded(raw, limit=65535):
        sample = dict(hex=raw.hex(), limit=limit, valid=False)
        try:
            sample['expected'] = sync_wire.decode(raw, limit)
            sample['valid'] = True
        except sync_wire.SyncError:
            pass
        cases['decode'].append(sample)

    for name in ('sync_v1_snapshot.hex', 'sync_v1_delta.hex'):
        raw = bytes.fromhex((ROOT / 'tests/fixtures' / name).read_text())
        decoded(raw)
        for length in range(len(raw)):
            decoded(raw[:length])
        decoded(raw + b'x')
        decoded(raw, len(raw) - 1)
        decoded(raw, -1)
    for offset, value in ((0, 0), (5, 2), (6, 3), (7, 1), (11, 0), (19, 0), (len(raw) - 1, 255)):
        changed = bytearray(raw); changed[offset] = value; decoded(bytes(changed))
    # All field tags, precision boundaries, NUL, non-BMP and exact UTF-8 limits.
    for integer in (-(2**63), -1, 0, 2**53 + 1, 2**63 - 1):
        wire = packet(sequence=2**64 - 1)
        wire['fields'] = {'1': 'alice', '2': integer, '3': -0.0, '4': False, '5': '中\0😀'}
        wire['types'] = {'1': 4, '2': 1, '3': 2, '4': 3, '5': 4}
        decoded(encode(wire))
    for number in (float('inf'), float('-inf'), float('nan')):
        wire = packet(); wire['fields']['10'] = number; wire['types']['10'] = 2
        decoded(encode(wire))
    for boolean in (False, True, 2, 255):
        wire = packet(); wire['fields']['10'] = boolean; wire['types']['10'] = 3
        decoded(encode(wire))
    for base, sequence in ((0, 1), (1, 3), (2**64-1, 1), (2**64-2, 2**64-1)):
        decoded(encode(packet(False, sequence, base)))
    for key in ('schema', 'stream', 'entity'):
        for value in ('', 'bad\0id', 'x' * 257, '中' * 86, '😀' * 64):
            wire = packet(); wire[key] = value; decoded(encode(wire))
    raw = encode(packet())
    field_start = 28 + sum(2 + len(packet()[key].encode()) for key in ('schema', 'stream', 'entity')) + 2
    for id_value in (0, 1):
        changed = bytearray(raw)
        # First field is the string key; second must not repeat it or use ID 0.
        second = field_start + 9 + len('alice')
        changed[second:second + 4] = struct.pack('>I', id_value)
        decoded(bytes(changed))
    changed = bytearray(raw); changed[field_start + 4] = 9; decoded(bytes(changed))
    for invalid in (b'\xc0\x80', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xff', b'\xe2\x82'):
        wire = packet(); wire['fields'] = {'1': ''}; wire['types'] = {'1': 4}
        raw_text = encode(wire)[:-4] + struct.pack('>I', len(invalid)) + invalid
        decoded(raw_text)
    changed = bytearray(raw); changed[field_start - 2:field_start] = struct.pack('>H', 1025); decoded(bytes(changed))
    for length in (65535, 65536):
        decoded(b'x' * length)
    for length in (1024, 1025):
        wire = packet(); wire['fields'] = {str(i): i for i in range(1, length + 1)}
        wire['types'] = {key: 1 for key in wire['fields']}; decoded(encode(wire))
    randomizer = random.Random(731)
    golden = bytes.fromhex((ROOT / 'tests/fixtures/sync_v1_snapshot.hex').read_text())
    for _ in range(200):
        changed = bytearray(golden)
        changed[randomizer.randrange(len(changed))] = randomizer.randrange(256)
        decoded(bytes(changed))

    def replica(owner=True, metadata=None, entity='alice'):
        metadata = copy.deepcopy(metadata or sync_wire.CLUSTER_SCHEMA)
        target = sync_wire.Replica(metadata, entity, owner)
        sample = dict(schema_json=json.dumps(metadata), key=next(f['id'] for f in metadata['fields'] if f['name'] == 'id'),
                      entity=entity, owner=owner, steps=[])
        cases['replicas'].append(sample)

        def apply(wire=None, reset=None):
            step = {}
            try:
                if reset is not None:
                    step['reset'] = reset; target.reset_stream(reset); step['status'] = 'reset'
                else:
                    raw = wire if isinstance(wire, bytes) else encode(wire)
                    step['hex'] = raw.hex()
                    step['status'] = {'applied': 'Applied', 'stale': 'Stale'}[target.apply(raw)]
            except sync_wire.SyncError as error:
                step['status'] = 'NeedsSnapshot' if str(error).startswith(('Stream mismatch', 'Delta baseline missing')) else 'invalid'
            step.update(values=dict(target.values), sequence=target.sequence, ready=target.ready)
            sample['steps'].append(step)
        return apply

    apply = replica()
    apply(packet())  # Cannot authenticate from an arbitrary snapshot.
    apply(reset='gateway:1:7')
    apply(packet(False, 2, 1))
    empty = packet(False, 3, 2); empty['fields'] = {}; empty['types'] = {}; apply(empty)
    apply(packet())
    apply(packet(False, 2, 1))
    apply(packet(False, 2, 1))
    apply(packet())
    apply(packet(False, 4, 3))
    apply(packet(sequence=5))
    apply(packet(sequence=9, stream='wrong'))
    for mutation in range(12):
        wire = packet(False, 6, 5)
        if mutation == 0: wire['schema_version'] = 2
        elif mutation == 1: wire['entity'] = 'other'
        elif mutation == 2: wire['schema'] = 'OtherSchema'
        elif mutation == 3: wire['fields']['30'] = 'server'; wire['types']['30'] = 4
        elif mutation == 4: wire['fields']['10'] = True; wire['types']['10'] = 3
        elif mutation == 5: wire['fields']['10'] = 101
        elif mutation == 6: wire['fields']['20'] = -1; wire['types']['20'] = 1
        elif mutation == 7: wire['fields']['1'] = 'other'; wire['types']['1'] = 4
        elif mutation == 8: wire = packet(sequence=6); del wire['fields']['20']; del wire['types']['20']
        elif mutation == 9: wire['fields']['1'] = '中' * 22; wire['types']['1'] = 4
        elif mutation == 10: apply(encode(wire)[:-1]); continue
        elif mutation == 11: wire['fields']['99'] = 3; wire['types']['99'] = 1
        apply(wire)
    apply(reset='bad\0stream')
    apply(reset='replacement')
    apply(packet())
    apply(packet(sequence=2**64-1, stream='replacement'))
    apply(packet(sequence=2**64-1, stream='replacement'))
    apply(packet(False, 2**64-1, 2**64-2, stream='replacement'))
    observer = replica(False); observer(reset='gateway:1:7'); observer(packet())
    public = packet(); del public['fields']['20']; del public['types']['20']; observer(public)
    owner_delta = packet(False, 2, 1); owner_delta['fields']['20'] = 3; owner_delta['types']['20'] = 1; observer(owner_delta)
    observer(packet(False, 2, 1))
    # Schema integer bounds must not round to double, and key IDs are explicit.
    precision = {'name': 'Precision', 'version': 1, 'fields': [
        {'id': 7, 'name': 'id', 'type': 'string', 'visibility': 'public', 'max_bytes': 64},
        {'id': 10, 'name': 'score', 'type': 'int64', 'visibility': 'public', 'min': 2**53+1, 'max': 2**63-2},
        {'id': 11, 'name': 'speed', 'type': 'double', 'visibility': 'public', 'min': -0.5, 'max': 1.5},
        {'id': 12, 'name': 'text', 'type': 'string', 'visibility': 'public', 'max_bytes': 8}]}
    exact = replica(metadata=precision); exact(reset='gateway:1:7')
    for sequence, (score, speed, text) in enumerate(((2**53, 0.5, '中\0😀'), (2**63-1, 0.5, ''), (2**53+1, 1.6, ''),
                                                    (2**53+1, 0.5, '中中中'), (2**53+1, -0.0, '中\0😀'), (2**63-2, 1.5, '')), 1):
        wire = packet(sequence=sequence); wire['schema'] = 'Precision'
        wire['fields'] = {'7': 'alice', '10': score, '11': speed, '12': text}
        wire['types'] = {'7': 4, '10': 1, '11': 2, '12': 4}; exact(wire)
    fixture_schema = dict(name='WireFixture', version=1, fields=[
        dict(id=1, name='id', type='string', visibility='public'),
        dict(id=2, name='integer', type='int64', visibility='owner'),
        dict(id=3, name='number', type='double', visibility='public'),
        dict(id=4, name='boolean', type='bool', visibility='public'),
        dict(id=5, name='text', type='string', visibility='public', max_bytes=256)])
    fixture_replica = replica(metadata=fixture_schema, entity='entity'); fixture_replica(reset='stream')
    for name in ('sync_v1_snapshot.hex', 'sync_v1_delta.hex'):
        fixture_replica(bytes.fromhex((ROOT / 'tests/fixtures' / name).read_text()))
    # Structural metadata errors must be reported as SyncException.
    base = copy.deepcopy(sync_wire.CLUSTER_SCHEMA)
    variants = []
    for change in range(12):
        schema = copy.deepcopy(base)
        if change == 0: schema['version'] = 0
        elif change == 1: schema['fields'].append(copy.deepcopy(schema['fields'][0]))
        elif change == 2: schema['fields'][0]['visibility'] = 'owner'
        elif change == 3: schema['fields'][0]['type'] = 'int64'
        elif change == 4: schema['fields'][1]['type'] = 'unknown'
        elif change == 5: schema['fields'][1]['visibility'] = 'server'
        elif change == 6: schema['fields'][1]['min'] = 101
        elif change == 7: schema['fields'][1]['min'] = 2**63
        elif change == 8: schema['fields'][1]['max_bytes'] = 2
        elif change == 9: schema['fields'] = []
        elif change == 10: schema['fields'][0]['max_bytes'] = -1
        elif change == 11: schema['fields'][1]['name'] = 'id'
        variants.append(json.dumps(schema))
    variants += ['null', '[]', '{}', '{', json.dumps(base).replace('"version": 1', '"version": 1, "version": 2')]
    cases['invalid_schemas'] = [dict(json=value, key=1) for value in variants]
    cases['invalid_schemas'].append(dict(json=json.dumps(base), key=99))
    return cases


class CSharpSyncTests(unittest.TestCase):
    def test_compiled_decoder_and_replica(self):
        if not shutil.which('dotnet'):
            if OPTIONS.require_dotnet: self.fail('dotnet SDK is required')
            self.skipTest('dotnet SDK not installed')
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            runtime = directory / 'runtime'
            shutil.copytree(OPTIONS.client_directory, runtime, ignore=shutil.ignore_patterns('bin', 'obj'))
            app = directory / 'app'; app.mkdir()
            shutil.copy(ROOT / 'tests/csharp_sync/Program.cs', app)
            schema = tool.load_schema(ROOT / 'examples/cluster_reference/player.schema.json')
            (app / 'Player.cs').write_text(tool.generate_csharp(schema), encoding='utf-8')
            (app / 'Test.csproj').write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><Nullable>enable</Nullable>
<TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup><ItemGroup>
<ProjectReference Include="../runtime/Chwell.Sync.csproj" /></ItemGroup></Project>''')
            data = directory / 'cases.json'; data.write_text(json.dumps(corpus(), ensure_ascii=True), encoding='utf-8')
            result = subprocess.run(['dotnet', 'run', '--project', str(app / 'Test.csproj'), '--configuration', 'Release', '--', str(data)],
                                    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
