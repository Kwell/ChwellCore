"""Runtime package generator contracts and optional installed C++ demonstration."""
import argparse
import copy
import importlib.util
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--tool', type=pathlib.Path, default=ROOT / 'tools/entity_schema.py')
parser.add_argument('--reference', type=pathlib.Path)
options, remaining = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('runtime_tool', options.tool)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class RuntimeContentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = pathlib.Path(temporary.name).resolve()
        self.content = self.directory / 'content'
        shutil.copytree(ROOT / 'examples/reference_service/content', self.content)
        self.catalog = self.content / 'catalog.json'

    def generate(self, output, version, *extra):
        return subprocess.run([sys.executable, str(options.tool), '--catalog', str(self.catalog),
                               '--bundle-output', str(output), '--content-version', str(version), *extra],
                              capture_output=True, text=True, encoding='utf-8')

    def test_deterministic_package_and_independent_uint64_version(self):
        output = self.directory / 'version.chct'
        result = self.generate(output, 2**64-1)
        self.assertEqual(result.returncode, 0, result.stderr)
        first = output.read_bytes()
        self.assertEqual(struct.unpack('>4sHHQI', first[:20]), (b'CHCT', 1, 0, 2**64-1, 2))
        catalog = json.loads(self.catalog.read_text()); catalog['tables'].reverse()
        self.catalog.write_text(json.dumps(catalog))
        items = self.content / 'item.schema.json'
        schema = json.loads(items.read_text()); schema['fields'].reverse(); items.write_text(json.dumps(schema))
        csv = self.content / 'items.csv'
        lines = csv.read_text().splitlines(); csv.write_text('\n'.join([lines[0], *reversed(lines[1:])]) + '\n')
        result = self.generate(output, 2**64-1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), first)
        # Opaque compatibility identity includes defaults, reference policy and schema version.
        normalized = tool.load_schema(self.content / 'player.schema.json')
        changed = copy.deepcopy(normalized); changed['fields'][1]['default'] += 1
        self.assertNotEqual(tool.content_contract(normalized), tool.content_contract(changed))

    def test_invalid_graph_keeps_binary_and_code_outputs(self):
        bundle, header = self.directory / 'bundle.chct', self.directory / 'player.h'
        result = self.generate(bundle, 1, str(self.content / 'player.schema.json'), '--output', str(header))
        self.assertEqual(result.returncode, 0, result.stderr)
        originals = (bundle.read_bytes(), header.read_bytes())
        (self.content / 'items.csv').write_text('id,power\nremoved,10\n')
        result = self.generate(bundle, 2, str(self.content / 'player.schema.json'), '--output', str(header))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unresolved reference', result.stderr)
        self.assertEqual((bundle.read_bytes(), header.read_bytes()), originals)

    def test_invalid_versions_flags_and_size_limits(self):
        output = self.directory / 'content.chct'; output.write_bytes(b'previous')
        for version in (0, -1, 2**64):
            self.assertNotEqual(self.generate(output, version).returncode, 0)
            self.assertEqual(output.read_bytes(), b'previous')
        result = self.generate(output, 1, '--output', str(output), str(self.content / 'player.schema.json'))
        self.assertIn('different paths', result.stderr)
        result = self.generate(output, 1, '--csv', 'other.csv')
        self.assertNotEqual(result.returncode, 0)
        result = subprocess.run([sys.executable, str(options.tool), str(self.content / 'player.schema.json'),
                                 '--catalog', str(self.catalog), '--csv', 'other.csv', '--output', str(output)],
                                capture_output=True, text=True, encoding='utf-8')
        self.assertIn('mutually exclusive', result.stderr)
        tables = tool.load_tables(tool.catalog_entries(self.catalog))
        tables['players'][1][0]['secret'] = 'x' * (1024 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, 'string exceeds'): tool.generate_bundle(tables, 1)

    def test_installed_runtime_switch_and_rollback(self):
        if not options.reference: self.skipTest('reference executable not supplied')
        first, second, rejected = [self.directory / name for name in ('first.chct', 'second.chct', 'invalid.chct')]
        result = self.generate(first, 11)
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.content / 'items.csv').write_text('id,power\nstaff,19\n')
        (self.content / 'players.csv').write_text('id,level,gold,secret,weapon\nhero,5,100,server-only,staff\n')
        result = self.generate(second, 12)
        self.assertEqual(result.returncode, 0, result.stderr)
        tables = tool.load_tables(tool.catalog_entries(self.catalog))
        # An independently corrupted but structurally valid package must also fail runtime validation.
        tables['players'][1][0]['weapon'] = 'missing'
        rejected.write_bytes(tool.generate_bundle(tables, 13))
        result = subprocess.run([str(options.reference), '--content-check', str(first), str(second), str(rejected)],
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('unresolved reference', result.stdout)
        self.assertIn('versions=11,12,11 powers=10,19,10 generation=3', result.stdout)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
