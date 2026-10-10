"""Chwell sync payload v1 decoder and atomic client replica (standard library).

Transport framing/authentication are separate. Reset streams only on successful
authenticated login, never on receipt of an arbitrary snapshot with a new stream.
"""
import math
import struct

MAX_BYTES = 65535
UINT64_MAX = (1 << 64) - 1


class SyncError(ValueError):
    pass


def _identity(value):
    return isinstance(value, str) and 0 < len(value.encode('utf-8')) <= 256 and '\0' not in value


def decode(payload, max_bytes=MAX_BYTES):
    if not isinstance(payload, bytes) or len(payload) > min(max_bytes, MAX_BYTES):
        raise SyncError('Sync packet exceeds byte limit')
    offset = 0

    def take(length):
        nonlocal offset
        if length > len(payload) - offset:
            raise SyncError('Truncated sync packet')
        data = payload[offset:offset + length]
        offset += length
        return data

    def integer(width):
        return int.from_bytes(take(width), 'big')

    def text(width, limit):
        length = integer(width)
        if length > limit:
            raise SyncError('Invalid sync string length')
        try:
            return take(length).decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise SyncError('Invalid UTF-8') from error

    if take(4) != b'CHWS':
        raise SyncError('Invalid sync magic')
    if integer(2) != 1:
        raise SyncError('Unsupported sync protocol version')
    kind, flags = integer(1), integer(1)
    if kind not in (1, 2) or flags:
        raise SyncError('Invalid packet kind or reserved flags')
    version = integer(4)
    sequence, base = integer(8), integer(8)
    name, stream, entity = (text(2, 256) for _ in range(3))
    if not all(_identity(value) for value in (name, stream, entity)) or not version or not sequence:
        raise SyncError('Invalid packet identity, version or sequence')
    if (kind == 1 and base != 0) or (kind == 2 and (not base or base == UINT64_MAX or sequence != base + 1)):
        raise SyncError('Invalid snapshot/delta baseline')
    count = integer(2)
    if count > 1024:
        raise SyncError('Too many fields')
    fields, types, previous = {}, {}, 0
    for _ in range(count):
        field, tag = integer(4), integer(1)
        if field <= previous:
            raise SyncError('Field IDs must be unique and increasing')
        previous = field
        if tag == 1:
            value = int.from_bytes(take(8), 'big', signed=True)
        elif tag == 2:
            value, = struct.unpack('!d', take(8))
            if not math.isfinite(value):
                raise SyncError('Non-finite double')
        elif tag == 3:
            value = integer(1)
            if value > 1:
                raise SyncError('Invalid bool')
            value = bool(value)
        elif tag == 4:
            value = text(4, MAX_BYTES)
        else:
            raise SyncError('Unknown field type tag')
        fields[str(field)], types[str(field)] = value, tag
    if offset != len(payload):
        raise SyncError('Trailing sync bytes')
    return dict(schema=name, schema_version=version, stream=stream, sequence=sequence,
                base_sequence=base, entity=entity, snapshot=kind == 1, fields=fields, types=types)


class Replica:
    def __init__(self, metadata, entity, is_owner):
        self.metadata = metadata
        self.entity = entity
        self.is_owner = is_owner
        self.stream = None
        self.sequence = 0
        self.values = {}
        self.ready = False

    def reset_stream(self, stream):
        if not _identity(stream):
            raise SyncError('Invalid authenticated sync stream')
        self.stream = stream
        self.sequence = 0
        self.values = {}
        self.ready = False

    def apply(self, packet):
        # Public input is wire bytes: decode completely before changing state.
        wire = decode(packet)
        if wire['schema'] != self.metadata['name'] or wire['schema_version'] != self.metadata['version'] or wire['entity'] != self.entity:
            raise SyncError('Schema version or entity mismatch')
        allowed = {str(field['id']): field for field in self.metadata['fields']
                   if field['visibility'] == 'public' or (self.is_owner and field['visibility'] == 'owner')}
        tags = dict(int64=1, double=2, bool=3, string=4)
        for field, value in wire['fields'].items():
            definition = allowed.get(field)
            if definition is None or wire['types'][field] != tags[definition['type']]:
                raise SyncError('Unknown, forbidden or wrongly typed field')
            if definition['type'] in ('int64', 'double'):
                if ('min' in definition and value < definition['min']) or ('max' in definition and value > definition['max']):
                    raise SyncError('Numeric constraint violation')
            if definition['type'] == 'string' and len(value.encode('utf-8')) > definition.get('max_bytes', MAX_BYTES):
                raise SyncError('String constraint violation')
            if definition['name'] == 'id' and value != self.entity:
                raise SyncError('Entity key mismatch')
        if wire['snapshot'] and wire['fields'].keys() != allowed.keys():
            raise SyncError('Incomplete snapshot')
        if self.stream is None or wire['stream'] != self.stream:
            raise SyncError('Stream mismatch; authenticate and request a snapshot')
        if wire['sequence'] <= self.sequence:
            return 'stale'
        if not wire['snapshot'] and (not self.ready or wire['base_sequence'] != self.sequence):
            raise SyncError('Delta baseline missing; request a snapshot')
        pending = {} if wire['snapshot'] else dict(self.values)
        pending.update(wire['fields'])
        self.values = pending
        self.sequence = wire['sequence']
        self.ready = True
        return 'applied'


# Compiled client contract; public/owner fields only. The test checks this against
# the schema source. A changed schema version requires an updated client contract.
CLUSTER_SCHEMA = {
    'name': 'ClusterPlayer', 'version': 1,
    'fields': [
        {'id': 1, 'name': 'id', 'type': 'string', 'visibility': 'public', 'max_bytes': 64},
        {'id': 10, 'name': 'level', 'type': 'int64', 'visibility': 'public', 'min': 1, 'max': 100},
        {'id': 20, 'name': 'gold', 'type': 'int64', 'visibility': 'owner', 'min': 0, 'max': 1000000},
    ],
}
