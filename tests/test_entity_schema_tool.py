"""Contracts of the schema compiler and content importer (stdlib only)."""
import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('entity_schema', ROOT / 'tools/entity_schema.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class SchemaToolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = pathlib.Path(self.temporary.name)
        self.schema = tool.load_schema(ROOT / 'schemas/player.schema.json')

    def load(self, data):
        path = self.directory / 'schema.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        return tool.load_schema(path)

    def csv(self, text):
        path = self.directory / 'content.csv'
        path.write_text(text, encoding='utf-8')
        return tool.load_csv(path, self.schema)

    def test_reordering_does_not_change_ids_or_generated_output(self):
        reordered = copy.deepcopy(self.schema)
        reordered['fields'].reverse()
        self.assertEqual(tool.generate(self.load(reordered), []), tool.generate(self.schema, []))

    def test_invalid_schema_types_defaults_and_reserved_ids(self):
        for key, value in [('type', []), ('visibility', {}), ('default', 'bad'), ('id', 4), ('stored', 1), ('min', 2**10000)]:
            schema = copy.deepcopy(self.schema)
            schema['fields'][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.load(schema)
        schema = copy.deepcopy(self.schema)
        schema['fields'][4]['max'] = 10**400
        with self.assertRaises(ValueError): self.load(schema)
        for key, value in [('visibility', 'server'), ('visibility', 'owner'), ('default', 'fixed'), ('max_bytes', 0)]:
            schema = copy.deepcopy(self.schema); schema['fields'][0][key] = value
            with self.subTest(identity=key, value=value), self.assertRaises(ValueError): self.load(schema)

    def test_duplicate_json_keys_fail(self):
        path = self.directory / 'schema.json'
        path.write_text('{"name":"A","name":"B"}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'duplicate JSON key'): tool.load_schema(path)

    def test_evolution_requires_reserving_deleted_id_and_bumping_version(self):
        new = copy.deepcopy(self.schema)
        new['fields'] = [f for f in new['fields'] if f['id'] != 30]
        new['version'] = 2
        with self.assertRaisesRegex(ValueError, 'must be reserved'): tool.check_evolution(new, self.schema, 'new')
        new['reserved_ids'].append(30)
        tool.check_evolution(new, self.schema, 'new')
        new['version'] = 1
        with self.assertRaisesRegex(ValueError, 'new version'): tool.check_evolution(new, self.schema, 'new')

    def test_evolution_rejects_id_reuse_policy_change_and_name_move(self):
        for key, value in [('type', 'double'), ('visibility', 'server'), ('stored', False), ('max', 200)]:
            new = copy.deepcopy(self.schema); new['version'] = 2
            new['fields'][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): tool.check_evolution(new, self.schema, 'new')
        new = copy.deepcopy(self.schema); new['version'] = 2
        new['fields'][1]['id'] = 11; new['reserved_ids'].append(10)
        with self.assertRaisesRegex(ValueError, 'another ID'): tool.check_evolution(new, self.schema, 'new')
        new = copy.deepcopy(self.schema); new['reserved_ids'] = []; new['version'] = 2
        with self.assertRaisesRegex(ValueError, 'remain reserved'): tool.check_evolution(new, self.schema, 'new')

    def test_new_field_is_compatible_with_version_bump(self):
        new = copy.deepcopy(self.schema); new['version'] = 2
        new['fields'].append({'id': 60, 'name': 'title', 'type': 'string', 'default': '', 'stored': True, 'visibility': 'public'})
        tool.check_evolution(self.load(new), self.schema, 'new')

    def test_csv_defaults_unicode_and_file_line_column_errors(self):
        rows = self.csv('id,level\n玩家,9\n')
        self.assertEqual(rows[0]['gold'], 0)
        self.assertEqual(rows[0]['id'], '玩家')
        for value in ['101', 'abc', '9223372036854775808', '+1', '01']:
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, r'content.csv:2:2 \(level\)'):
                self.csv('id,level\np,' + value + '\n')
        with self.assertRaisesRegex(ValueError, 'duplicate content ID'): self.csv('id\np\np\n')
        with self.assertRaisesRegex(ValueError, 'unknown CSV column'): self.csv('id,typo\np,1\n')
        with self.assertRaisesRegex(ValueError, 'expected true or false'): self.csv('id,online\np,1\n')
        with self.assertRaisesRegex(ValueError, 'finite double'): self.csv('id,speed\np,1e309\n')

    def test_utf8_byte_constraint(self):
        with self.assertRaisesRegex(ValueError, 'max_bytes'): self.csv('id\n' + '玩' * 22 + '\n')

    def test_failed_cli_preserves_previous_artifact(self):
        schema = self.directory / 'schema.json'; schema.write_text(json.dumps(self.schema), encoding='utf-8')
        csv = self.directory / 'data.csv'; csv.write_text('id,level\np,999\n', encoding='utf-8')
        output = self.directory / 'generated.h'; output.write_bytes(b'previous artifact\n')
        command = [sys.executable, str(ROOT / 'tools/entity_schema.py'), str(schema), '--csv', str(csv), '--output', str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(':2:2 (level)', result.stderr)
        self.assertEqual(output.read_bytes(), b'previous artifact\n')
        csv.write_text('id,level\np,9\n', encoding='utf-8')
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('class SchemaPlayer', output.read_text(encoding='utf-8'))

    def test_generated_symbols_and_server_metadata(self):
        schema = copy.deepcopy(self.schema); schema['name'] = 'content'
        with self.assertRaisesRegex(ValueError, 'conflicts'): self.load(schema)
        generated = tool.generate(self.schema, [])
        client = generated.split('client_schema()')[1].split('content()')[0]
        self.assertNotIn('secret', client)
        schema['name'] = 'schema'; schema['fields'][1]['name'] = 'value'
        generated = tool.generate(self.load(schema), [])
        self.assertIn('::chwell::schema::SchemaEntity::set_value(field_value', generated)

    def test_namespace_tokens_in_data_are_preserved(self):
        schema = copy.deepcopy(self.schema)
        schema['fields'][3]['default'] = 'std::abc schema::xyz'
        output = tool.generate(schema, [])
        self.assertIn('"std::abc schema::xyz"', output)
        self.assertNotIn('"::std::abc', output)

    def catalog(self, tables):
        entries = []
        for schema, text in tables:
            definition = self.directory / (schema['table'] + '.json')
            content = self.directory / (schema['table'] + '.csv')
            definition.write_text(json.dumps(schema), encoding='utf-8')
            content.write_text(text, encoding='utf-8')
            entries.append({'schema': definition.name, 'csv': content.name})
        manifest = self.directory / 'catalog.json'
        manifest.write_text(json.dumps({'tables': entries}), encoding='utf-8')
        return manifest, self.directory / entries[0]['schema']

    def reference_schema(self, table='items', allow_empty=False):
        schema = copy.deepcopy(self.schema)
        schema['fields'][3]['reference'] = {'table': table, 'allow_empty': allow_empty}
        return schema

    def test_catalog_references_defaults_and_private_metadata(self):
        source = self.reference_schema()
        source['fields'][3]['default'] = 'sword'
        items = copy.deepcopy(self.schema); items['table'] = 'items'
        manifest, path = self.catalog([(source, 'id\nhero\n'), (items, 'id\nsword\n')])
        normalized = tool.load_schema(path)
        rows = tool.load_content(path, normalized, catalog_path=manifest)
        self.assertEqual(rows[0]['secret'], 'sword')
        # Even public field references are server-side content metadata.
        source['fields'][3]['visibility'] = 'public'
        output = tool.generate(self.load(source), rows)
        client = output.split('client_schema()')[1].split('content()')[0]
        self.assertNotIn('reference', client)
        self.assertNotIn('items', client)

    def test_reference_diagnostics_track_multiline_cells_and_omitted_defaults(self):
        source = self.reference_schema()
        items = copy.deepcopy(self.schema); items['table'] = 'items'
        for text, diagnostic in [('id,secret\n"hero\nsecond",bad\n', r'players.csv:2:2 \(secret\)'),
                                 ('id\nhero\n', r'players.csv:2:1 \(secret; omitted column, schema default\)')]:
            manifest, path = self.catalog([(source, text), (items, 'id\nsword\n')])
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, diagnostic):
                tool.load_content(path, tool.load_schema(path), catalog_path=manifest)

    def test_self_references_cycles_and_explicit_empty_policy(self):
        source = self.reference_schema('players', True)
        manifest, path = self.catalog([(source, 'id,secret\na,b\nb,a\nc,\n')])
        self.assertEqual(len(tool.load_content(path, tool.load_schema(path), catalog_path=manifest)), 3)
        source = self.reference_schema('items')
        items = self.reference_schema('players'); items['table'] = 'items'
        manifest, path = self.catalog([(source, 'id,secret\na,b\n'), (items, 'id,secret\nb,a\n')])
        tool.load_content(path, tool.load_schema(path), catalog_path=manifest)
        # Validate the unselected table too, and allow_empty does not allow typos.
        (self.directory / 'items.csv').write_text('id,secret\nb,missing\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, r'items.csv:2:2.*unresolved reference'):
            tool.load_content(path, tool.load_schema(path), catalog_path=manifest)

    def test_reference_schema_and_evolution_policy(self):
        for field, reference in [(0, {'table': 'items'}), (1, {'table': 'items'}),
                                 (3, {'table': 'items', 'allow_empty': 1}), (3, {'table': []}),
                                 (3, {'table': 'items', 'typo': True})]:
            schema = copy.deepcopy(self.schema); schema['fields'][field]['reference'] = reference
            with self.subTest(reference=reference), self.assertRaises(ValueError): self.load(schema)
        old = self.load(self.reference_schema())
        new = copy.deepcopy(old); new['version'] += 1
        new['fields'][3]['reference']['allow_empty'] = True
        with self.assertRaisesRegex(ValueError, 'reference'): tool.check_evolution(new, old, 'schema')

    def test_catalog_rejects_missing_duplicate_and_unselected_inputs(self):
        source = self.reference_schema()
        manifest, path = self.catalog([(source, 'id,secret\na,sword\n')])
        with self.assertRaisesRegex(ValueError, 'missing reference table'):
            tool.load_content(path, tool.load_schema(path), catalog_path=manifest)
        items = copy.deepcopy(self.schema); items['table'] = 'items'
        manifest, path = self.catalog([(source, 'id,secret\na,sword\n'), (items, 'id\nsword\nsword\n')])
        with self.assertRaisesRegex(ValueError, 'duplicate content ID'):
            tool.load_content(path, tool.load_schema(path), catalog_path=manifest)
        tool.load_schema(path)
        with self.assertRaisesRegex(ValueError, 'mutually exclusive'):
            tool.load_content(path, source, csv_path='other.csv', catalog_path=manifest)
        manifest, path = self.catalog([(self.schema, 'id\na\n')])
        data = json.loads(manifest.read_text()); data['tables'].append(data['tables'][0])
        manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'duplicate catalog schema'): tool.catalog_inputs(manifest)
        # Different schemas cannot claim the same table name.
        data['tables'][1] = {'schema': 'other.json', 'csv': 'players.csv'}
        (self.directory / 'other.json').write_text(json.dumps(self.schema))
        manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'duplicate catalog table'):
            tool.load_content(path, self.schema, catalog_path=manifest)
        data['tables'] = data['tables'][1:]; manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'not in catalog'):
            tool.load_content(path, self.schema, catalog_path=manifest)

    def test_catalog_cli_failure_is_atomic_and_lists_all_dependencies(self):
        source = self.reference_schema('players', True)
        manifest, path = self.catalog([(source, 'id,secret\na,typo\n')])
        output = self.directory / 'generated.h'; output.write_bytes(b'previous\n')
        command = [sys.executable, str(ROOT / 'tools/entity_schema.py'), str(path),
                   '--catalog', str(manifest), '--output', str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unresolved reference', result.stderr)
        self.assertEqual(output.read_bytes(), b'previous\n')
        (self.directory / 'players.csv').write_text('id,secret\na,\n')
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        listed = subprocess.run([sys.executable, str(ROOT / 'tools/entity_schema.py'),
                                 '--catalog', str(manifest), '--list-inputs'], capture_output=True, text=True)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertEqual(listed.stdout.splitlines(), [p.as_posix() for p in tool.catalog_inputs(manifest)[0]])


if __name__ == '__main__':
    unittest.main()
