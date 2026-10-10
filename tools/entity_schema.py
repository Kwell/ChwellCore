#!/usr/bin/env python3
"""Validate a schema/content table and atomically generate one C++ header.

No third-party Python dependencies. Build with --previous to enforce evolution
against a committed prior schema. Server-only metadata never enters client_schema.
"""
import argparse
import csv
import json
import math
import os
import pathlib
import re
import tempfile

KEYWORDS = set('alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break case catch char char16_t char32_t class compl concept const constexpr const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch synchronized template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq'.split())
TYPE_MAP = {'int64': ('std::int64_t', 'Int64'), 'double': ('double', 'Double'),
            'bool': ('bool', 'Bool'), 'string': ('std::string', 'String')}
VISIBILITY = {'server': 'Server', 'owner': 'Owner', 'public': 'Public'}


def error(where, message):
    raise ValueError(f'{where}: {message}')


def identifier(value, where):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', value) or value in KEYWORDS or '__' in value:
        error(where, 'expected a non-reserved C++ identifier')


def integer(value, low, high, where):
    if type(value) is not int or not low <= value <= high:
        error(where, f'expected integer in [{low}, {high}]')


def checked_value(field, value, where):
    kind = field['type']
    if kind == 'int64':
        integer(value, -(2**63), 2**63 - 1, where)
    elif kind == 'double':
        if type(value) not in (float, int): error(where, 'expected finite double')
        try: value = float(value)
        except OverflowError: error(where, 'double overflow')
        if not math.isfinite(value): error(where, 'expected finite double')
    elif kind == 'bool':
        if type(value) is not bool: error(where, 'expected boolean')
    elif not isinstance(value, str):
        error(where, 'expected string')
    if kind in ('int64', 'double'):
        if 'min' in field and value < field['min']: error(where, 'below minimum')
        if 'max' in field and value > field['max']: error(where, 'above maximum')
    if kind == 'string':
        try: size = len(value.encode('utf-8'))
        except UnicodeError: error(where, 'invalid Unicode string')
        if 'max_bytes' in field and size > field['max_bytes']: error(where, 'string exceeds max_bytes')
    return value


def load_json(path):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result: error(path, f'duplicate JSON key {key}')
            result[key] = value
        return result
    try:
        data = json.loads(pathlib.Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_pairs)
    except (OSError, json.JSONDecodeError) as exc:
        error(path, str(exc))
    return data


def load_schema(path):
    data = load_json(path)
    if not isinstance(data, dict): error(path, 'schema must be an object')
    allowed = {'name', 'table', 'key', 'version', 'fields', 'reserved_ids'}
    if set(data) - allowed: error(path, 'unknown schema keys: ' + ', '.join(sorted(set(data) - allowed)))
    for key in ('name', 'table', 'key', 'version', 'fields'):
        if key not in data: error(path, f'missing {key}')
    identifier(data['name'], f'{path}:name')
    identifier(data['table'], f'{path}:table')
    integer(data['key'], 1, 2**32 - 1, f'{path}:key')
    integer(data['version'], 1, 2**32 - 1, f'{path}:version')
    if not isinstance(data['fields'], list) or not data['fields']: error(path, 'fields must be a nonempty array')
    reserved = data.setdefault('reserved_ids', [])
    if not isinstance(reserved, list): error(path, 'reserved_ids must be an array')
    used_ids, names = set(), set()
    for field_id in reserved:
        integer(field_id, 1, 2**32 - 1, f'{path}:reserved_ids')
        if field_id in used_ids: error(path, 'duplicate reserved ID')
        used_ids.add(field_id)
    for index, field in enumerate(data['fields']):
        where = f'{path}:fields[{index}]'
        if not isinstance(field, dict): error(where, 'field must be an object')
        if set(field) - {'id', 'name', 'type', 'default', 'stored', 'visibility', 'min', 'max', 'max_bytes', 'reference'}: error(where, 'unknown field keys')
        for key in ('id', 'name', 'type', 'default', 'stored', 'visibility'):
            if key not in field: error(where, f'missing {key}')
        integer(field['id'], 1, 2**32 - 1, where + ':id')
        identifier(field['name'], where + ':name')
        if field['id'] in used_ids or field['name'] in names: error(where, 'duplicate/reserved ID or duplicate name')
        used_ids.add(field['id']); names.add(field['name'])
        if not isinstance(field['type'], str) or field['type'] not in TYPE_MAP: error(where, 'unsupported field type')
        if not isinstance(field['visibility'], str) or field['visibility'] not in VISIBILITY: error(where, 'invalid visibility')
        if type(field['stored']) is not bool: error(where, 'stored must be boolean')
        if 'reference' in field:
            reference = field['reference']
            if field['type'] != 'string' or field['id'] == data['key']:
                error(where, 'reference requires a non-key string field')
            if not isinstance(reference, dict) or set(reference) - {'table', 'allow_empty'} or 'table' not in reference:
                error(where, 'reference requires table and optional allow_empty')
            identifier(reference['table'], where + ':reference:table')
            if type(reference.setdefault('allow_empty', False)) is not bool:
                error(where, 'reference allow_empty must be boolean')
        if ('min' in field or 'max' in field) and field['type'] not in ('int64', 'double'): error(where, 'numeric constraints on non-numeric field')
        if 'max_bytes' in field:
            if field['type'] != 'string': error(where, 'max_bytes on non-string field')
            integer(field['max_bytes'], 0, 2**32 - 1, where + ':max_bytes')
        for bound in ('min', 'max'):
            if bound not in field: continue
            if field['type'] == 'int64': integer(field[bound], -(2**63), 2**63 - 1, where + ':' + bound)
            else:
                field[bound] = checked_value({'type': 'double'}, field[bound], where + ':' + bound)
        if 'min' in field and 'max' in field and field['min'] > field['max']: error(where, 'min exceeds max')
        field['default'] = checked_value(field, field['default'], where + ':default')
    identity = next((field for field in data['fields'] if field['id'] == data['key']), None)
    if not identity or identity['name'] != 'id' or identity['type'] != 'string' or not identity['stored']:
        error(path, 'key must identify the stored string field named id')
    if identity['default'] != '': error(path, 'key default must be empty; set identity explicitly')
    if identity['visibility'] != 'public': error(path, 'key must be public because sync packets carry entity identity')
    if identity.get('max_bytes') == 0: error(path, 'key must allow a nonempty identity')
    data['fields'].sort(key=lambda field: field['id'])
    data['reserved_ids'].sort()
    symbols = {'entity_schema', 'schema_version', 'client_schema', 'content'}
    for field in data['fields']:
        symbols.update(prefix + field['name'] for prefix in ('get_', 'set_', 'field_'))
    if data['name'] in symbols: error(path, 'class name conflicts with generated member')
    return data


def check_evolution(current, previous, where):
    for key in ('name', 'table', 'key'):
        if current[key] != previous[key]: error(where, f'incompatible {key} change')
    old_fields = {field['id']: field for field in previous['fields']}
    new_fields = {field['id']: field for field in current['fields']}
    reserved = set(current['reserved_ids'])
    old_names = {field['name']: field['id'] for field in previous['fields']}
    for field in current['fields']:
        if field['name'] in old_names and old_names[field['name']] != field['id']:
            error(where, 'existing field name cannot move to another ID')
    if not set(previous['reserved_ids']) <= reserved: error(where, 'previous reserved IDs must remain reserved')
    for field_id, old in old_fields.items():
        if field_id not in new_fields:
            if field_id not in reserved: error(where, f'deleted field ID {field_id} must be reserved')
            continue
        new = new_fields[field_id]
        # Require an explicit migration for changing storage or visibility policy.
        for key in ('name', 'type', 'stored', 'visibility', 'min', 'max', 'max_bytes', 'reference'):
            if new.get(key) != old.get(key): error(where, f'incompatible field ID {field_id} change: {key}')
    if current['version'] < previous['version']: error(where, 'version went backwards')
    if current != previous and current['version'] <= previous['version']: error(where, 'schema changes require a new version')


def load_csv(path, schema, locations=None):
    fields = {field['name']: field for field in schema['fields']}
    rows, ids = [], set()
    with open(path, encoding='utf-8-sig', newline='') as source:
        reader = csv.reader(source, strict=True)
        try: header = next(reader)
        except StopIteration: error(path, 'empty CSV')
        if len(header) != len(set(header)): error(path, 'duplicate CSV column')
        if set(header) - set(fields): error(path, 'unknown CSV column')
        if 'id' not in header: error(path, 'missing id column')
        while True:
            line = reader.line_num + 1  # Start of record, including quoted multiline cells.
            try: row = next(reader)
            except StopIteration: break
            if len(row) != len(header): error(f'{path}:{line}', 'column count mismatch')
            values = {field['name']: field['default'] for field in schema['fields']}
            for column, (name, text) in enumerate(zip(header, row), 1):
                where = f'{path}:{line}:{column} ({name})'
                kind = fields[name]['type']
                if kind == 'int64':
                    if not re.fullmatch(r'-?(0|[1-9][0-9]*)', text): error(where, 'invalid integer')
                    value = int(text)
                elif kind == 'double':
                    if not re.fullmatch(r'-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?', text): error(where, 'invalid double')
                    value = float(text)
                elif kind == 'bool':
                    if text not in ('true', 'false'): error(where, 'expected true or false')
                    value = text == 'true'
                else: value = text
                values[name] = checked_value(fields[name], value, where)
            if not values['id'] or values['id'] in ids: error(f'{path}:{line}:{header.index("id") + 1}', 'empty or duplicate content ID')
            ids.add(values['id']); rows.append(values)
            if locations is not None:
                locations.append({name: f'{path}:{line}:{header.index(name) + 1} ({name})'
                                  if name in header else
                                  f'{path}:{line}:{header.index("id") + 1} ({name}; omitted column, schema default)'
                                  for name in fields})
    return rows


def catalog_inputs(path):
    """Resolve manifest paths without reading data, also used by CMake DEPENDS."""
    path = pathlib.Path(path).resolve()
    data = load_json(path)
    if not isinstance(data, dict) or set(data) != {'tables'} or not isinstance(data['tables'], list) or not data['tables']:
        error(path, 'catalog requires a nonempty tables array')
    inputs, seen = [], set()
    for index, entry in enumerate(data['tables']):
        where = f'{path}:tables[{index}]'
        if not isinstance(entry, dict) or set(entry) != {'schema', 'csv'}:
            error(where, 'table entry requires schema and csv paths')
        paths = []
        for key in ('schema', 'csv'):
            value = entry[key]
            if not isinstance(value, str) or not value or any(c in value for c in '\r\n;\x00'):
                error(where, 'expected nonempty path without newline, semicolon or NUL')
            resolved = (path.parent / value).resolve()
            if any(c in str(resolved) for c in '\r\n;'):
                error(where, 'resolved path cannot contain newline or semicolon')
            paths.append(resolved)
        if paths[0] in seen: error(where, 'duplicate catalog schema')
        seen.add(paths[0]); inputs.append(tuple(paths))
    return inputs


def load_content(schema_path, schema, csv_path=None, catalog_path=None):
    if catalog_path and csv_path: error(catalog_path, '--catalog and --csv are mutually exclusive')
    inputs = catalog_inputs(catalog_path) if catalog_path else [(pathlib.Path(schema_path).resolve(), csv_path)]
    tables, selected = {}, None
    # Load all tables first, so self-references and cycles need no recursion.
    for definition, content in inputs:
        current = schema if definition == pathlib.Path(schema_path).resolve() else load_schema(definition)
        table = current['table']
        if table in tables: error(definition, f'duplicate catalog table {table}')
        locations = []
        rows = load_csv(content, current, locations) if content else []
        tables[table] = (current, rows, locations, definition, {row['id'] for row in rows})
        if definition == pathlib.Path(schema_path).resolve(): selected = rows
    if selected is None: error(catalog_path, 'requested schema is not in catalog')
    for current, rows, locations, definition, _ in tables.values():
        for field in current['fields']:
            if 'reference' not in field: continue
            reference = field['reference']
            target = reference['table']
            if target not in tables:
                error(f'{definition}:field {field["name"]}', f'missing reference table {target}; supply --catalog')
            ids = tables[target][4]
            for row, location in zip(rows, locations):
                value = row[field['name']]
                if value == '' and reference['allow_empty']: continue
                if value not in ids:
                    error(location[field['name']], f'unresolved reference {target}.id = {value!r}')
    return selected


def cpp_string(value):
    raw = value.encode('utf-8')
    text = ''.join(chr(c) if 32 <= c <= 126 and c not in (34, 92) else '\\%03o' % c for c in raw)
    return f'::std::string("{text}", {len(raw)})'


def cpp_value(kind, value):
    if kind == 'string': return cpp_string(value)
    if kind == 'bool': return 'true' if value else 'false'
    if kind == 'int64':
        if value == -(2**63): return '::std::numeric_limits<::std::int64_t>::min()'
        return f'::std::int64_t{{{value}LL}}'
    return f'double{{{repr(float(value))}}}'


def generate(schema, rows):
    name = schema['name']
    lines = ['// Generated by tools/entity_schema.py; do not edit.', '#pragma once',
             '#include "chwell/schema/entity_schema.h"', '#include <limits>', '#include <stdexcept>',
             'namespace chwell { namespace generated {',
             f'class {name} : public ::chwell::schema::SchemaEntity {{', 'public:',
             f'    {name}() : ::chwell::schema::SchemaEntity(entity_schema()) {{}}',
             '    static ::std::shared_ptr<const ::chwell::schema::EntitySchema> entity_schema() {',
             '        static const auto definition = [] {', '            ::std::vector<::chwell::schema::FieldDefinition> fields;']
    for field in schema['fields']:
        lines += ['            {', '                ::chwell::schema::FieldDefinition field;',
                  f'                field.id = {field["id"]}u;', f'                field.name = {cpp_string(field["name"])};',
                  f'                field.type = ::chwell::schema::FieldType::{TYPE_MAP[field["type"]][1]};',
                  f'                field.default_value = {cpp_value(field["type"], field["default"])};',
                  f'                field.stored = {str(field["stored"]).lower()};',
                  f'                field.visibility = ::chwell::schema::Visibility::{VISIBILITY[field["visibility"]]};']
        for bound in ('min', 'max'):
            if bound in field:
                target = 'int_' if field['type'] == 'int64' else 'double_'
                lines.append(f'                field.{target}{bound} = {cpp_value(field["type"], field[bound])};')
        if 'max_bytes' in field: lines.append(f'                field.max_bytes = {field["max_bytes"]}u;')
        lines += ['                fields.push_back(::std::move(field));', '            }']
    reserved = ', '.join(str(field_id) + 'u' for field_id in schema['reserved_ids'])
    lines += [f'            return ::std::make_shared<const ::chwell::schema::EntitySchema>({cpp_string(name)}, {cpp_string(schema["table"])}, {schema["key"]}u, ::std::move(fields), ::std::vector<::chwell::schema::FieldId>{{{reserved}}});',
              '        }();', '        return definition;', '    }', f'    static constexpr ::std::uint32_t schema_version = {schema["version"]}u;']
    for field in schema['fields']:
        key, cpp_type = field['name'], TYPE_MAP[field['type']][0]
        if cpp_type.startswith('std::'): cpp_type = '::' + cpp_type
        lines += [f'    static constexpr ::chwell::schema::FieldId field_{key} = {field["id"]}u;',
                  f'    const {cpp_type}& get_{key}() const {{ return ::std::get<{cpp_type}>(::chwell::schema::SchemaEntity::value(field_{key})); }}',
                  f'    bool set_{key}({cpp_type} input, ::std::string* error = nullptr) {{ return ::chwell::schema::SchemaEntity::set_value(field_{key}, ::std::move(input), error); }}']
    client = {'name': name, 'version': schema['version'], 'reserved_ids': schema['reserved_ids'],
              'fields': [{key: value for key, value in field.items() if key != 'reference'}
                         for field in schema['fields'] if field['visibility'] != 'server']}
    client_text = json.dumps(client, ensure_ascii=True, sort_keys=True, separators=(',', ':'))
    lines += [f'    static ::std::string client_schema() {{ return {cpp_string(client_text)}; }}',
              f'    static ::std::vector<{name}> content() {{', f'        ::std::vector<{name}> rows;']
    for row in rows:
        lines += ['        {', f'            {name} row;']
        for field in schema['fields']:
            lines.append(f'            if (!row.set_{field["name"]}({cpp_value(field["type"], row[field["name"]])})) throw ::std::logic_error("Generated content failed validation");')
        lines += ['            row.clear_dirty();', '            rows.push_back(::std::move(row));', '        }']
    lines += ['        return rows;', '    }', '};', '} } // namespace chwell::generated', '']
    return '\n'.join(lines)


def atomic_write(path, content):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n', dir=path.parent, delete=False) as target:
            temporary = pathlib.Path(target.name)
            target.write(content)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and temporary.exists(): temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('schema', nargs='?')
    parser.add_argument('--previous')
    parser.add_argument('--csv')
    parser.add_argument('--catalog', help='JSON manifest of schema/CSV pairs; validate the entire graph')
    parser.add_argument('--list-inputs', action='store_true', help='list catalog dependencies for build systems')
    parser.add_argument('--output')
    args = parser.parse_args()
    try:
        if args.list_inputs:
            if not args.catalog or args.schema or args.output or args.csv or args.previous:
                error('arguments', '--list-inputs requires only --catalog')
            for pair in catalog_inputs(args.catalog):
                for path in pair: print(path.as_posix())
            return
        if not args.schema or not args.output: error('arguments', 'schema and --output are required')
        schema = load_schema(args.schema)
        if args.previous: check_evolution(schema, load_schema(args.previous), args.schema)
        rows = load_content(args.schema, schema, args.csv, args.catalog)
        atomic_write(args.output, generate(schema, rows))
    except (ValueError, OSError, csv.Error) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
