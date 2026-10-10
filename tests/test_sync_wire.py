import importlib.util
import json
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "chwell_sync_wire", ROOT / "examples" / "cluster_reference" / "sync_wire.py"
)
sync_wire = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync_wire)
CLIENT_SPEC = importlib.util.spec_from_file_location(
    'cluster_client', ROOT / 'examples/cluster_reference/client.py')
client_module = importlib.util.module_from_spec(CLIENT_SPEC)
CLIENT_SPEC.loader.exec_module(client_module)


def packet(snapshot=True, sequence=1, base=0, entity="alice", stream="gateway:1:7"):
    fields = {"1": entity, "10": 1}
    types = {"1": 4, "10": 1}
    if snapshot:
        fields["20"] = 3
        types["20"] = 1
    return {
        "schema": "ClusterPlayer", "schema_version": 1, "stream": stream,
        "sequence": sequence, "base_sequence": base, "entity": entity,
        "snapshot": snapshot, "fields": fields, "types": types,
    }


def encode(wire):
    out = bytearray(b"CHWS")
    out += struct.pack(">HBBIQQ", 1, 1 if wire["snapshot"] else 2, 0,
                       wire["schema_version"], wire["sequence"], wire["base_sequence"])
    for value in (wire["schema"], wire["stream"], wire["entity"]):
        raw = value.encode("utf-8")
        out += struct.pack(">H", len(raw)) + raw
    fields = sorted((int(key), value, wire["types"][key]) for key, value in wire["fields"].items())
    out += struct.pack(">H", len(fields))
    for field, value, tag in fields:
        out += struct.pack(">IB", field, tag)
        if tag == 1:
            out += struct.pack(">q", value)
        elif tag == 2:
            out += struct.pack(">d", value)
        elif tag == 3:
            out += bytes((value,))
        else:
            raw = value.encode("utf-8")
            out += struct.pack(">I", len(raw)) + raw
    return bytes(out)


class SyncWireTests(unittest.TestCase):
    def test_cpp_golden_payloads_and_truncated_prefixes(self):
        snapshot = bytes.fromhex((ROOT / 'tests/fixtures/sync_v1_snapshot.hex').read_text())
        delta = bytes.fromhex((ROOT / 'tests/fixtures/sync_v1_delta.hex').read_text())
        decoded = sync_wire.decode(snapshot)
        self.assertEqual(decoded['fields'], {'1': 'entity', '2': -(1 << 63),
                                            '3': 0.1, '4': True, '5': '中\0x'})
        self.assertEqual(sync_wire.decode(delta)['fields'], {'2': (1 << 63) - 1})
        self.assertEqual(encode(decoded), snapshot)
        self.assertEqual(encode(sync_wire.decode(delta)), delta)
        for payload in (snapshot, delta):
            for length in range(len(payload)):
                with self.assertRaises(sync_wire.SyncError):
                    sync_wire.decode(payload[:length])
            with self.assertRaises(sync_wire.SyncError):
                sync_wire.decode(payload + b'x')
        for offset, value in ((5, 2), (6, 3), (7, 1), (len(snapshot) - 1, 255)):
            malformed = bytearray(snapshot)
            malformed[offset] = value
            with self.assertRaises(sync_wire.SyncError):
                sync_wire.decode(bytes(malformed))

    def test_client_metadata_matches_source_schema(self):
        source = json.loads((ROOT / 'examples/cluster_reference/player.schema.json').read_text())
        contract = sync_wire.CLUSTER_SCHEMA
        self.assertEqual((source['name'], source['version']), (contract['name'], contract['version']))
        visible = [field for field in source['fields'] if field['visibility'] != 'server']
        self.assertEqual(len(visible), len(contract['fields']))
        for definition, compiled in zip(visible, contract['fields']):
            self.assertEqual(compiled, {key: definition[key] for key in compiled})

    def test_decode_and_uint64(self):
        wire = packet(sequence=(1 << 64) - 1, base=0)
        decoded = sync_wire.decode(encode(wire))
        self.assertEqual(decoded["sequence"], (1 << 64) - 1)
        self.assertEqual(decoded["fields"]["1"], "alice")

    def test_replica_snapshot_delta_and_gap(self):
        replica = sync_wire.Replica(sync_wire.CLUSTER_SCHEMA, "alice", True)
        replica.reset_stream("gateway:1:7")
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(encode(packet(False, 2, 1)))
        self.assertEqual(replica.apply(encode(packet())), "applied")
        self.assertEqual(replica.apply(encode(packet(False, 2, 1))), "applied")
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(encode(packet(False, 4, 3)))
        self.assertEqual(replica.sequence, 2)
        self.assertEqual(replica.apply(encode(packet(False, 2, 1))), 'stale')
        self.assertEqual(replica.apply(encode(packet(sequence=5))), 'applied')
        replica.reset_stream('gateway:1:8')
        self.assertFalse(replica.ready)
        self.assertEqual(replica.values, {})
        self.assertEqual(replica.apply(encode(packet(stream='gateway:1:8'))), 'applied')

    def test_owner_field_and_incomplete_snapshot_rejected_atomically(self):
        replica = sync_wire.Replica(sync_wire.CLUSTER_SCHEMA, 'alice', False)
        replica.reset_stream('gateway:1:7')
        wire = packet()
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(encode(wire))
        del wire['fields']['20']
        self.assertEqual(replica.apply(encode(wire)), 'applied')
        before = (dict(replica.values), replica.sequence)
        wire['sequence'] = 2
        del wire['fields']['10']
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(encode(wire))
        self.assertEqual((replica.values, replica.sequence), before)

    def test_invalid_inputs_do_not_mutate_replica(self):
        replica = sync_wire.Replica(sync_wire.CLUSTER_SCHEMA, "alice", True)
        replica.reset_stream("gateway:1:7")
        self.assertEqual(replica.apply(encode(packet())), "applied")
        before = (dict(replica.values), replica.sequence)
        malformed = bytearray(encode(packet(False, 2, 1)))
        malformed[-1] = 0xff
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(bytes(malformed))
        self.assertEqual((replica.values, replica.sequence), before)
        replica.reset_stream("gateway:1:8")
        with self.assertRaises(sync_wire.SyncError):
            replica.apply(encode(packet(False, 2, 1, stream="gateway:1:7")))


class ClientTests(unittest.TestCase):
    def test_framed_responses_delta_gap_snapshot_and_relogin(self):
        class Socket:
            incoming = b''
            sent = []

            def sendall(self, data):
                self.sent.append(data)

            def recv(self, length):
                data, self.incoming = self.incoming[:length], self.incoming[length:]
                return data

            def feed(self, wire):
                body = json.dumps({'ok': True, 'packet': {
                    'encoding': 'chwell-sync-v1-hex', 'data': encode(wire).hex()}}).encode()
                self.incoming = struct.pack('!HH', 1, len(body)) + body

        client = client_module.Client.__new__(client_module.Client)
        client.socket = Socket()
        client.player = client.stream = None
        client.replicas = {}
        client.socket.feed(packet())
        result = client.ask('login', player='alice', token='test-token')
        self.assertEqual(result['packet']['sequence'], '1')
        request = json.loads(client.socket.sent[-1][4:])
        self.assertEqual((request['sync_version'], request['schema_version']), (1, 1))
        client.socket.feed(packet(False, 2, 1))
        self.assertEqual(client.ask('advance')['packet']['base_sequence'], '1')
        before = dict(client.replicas['alice'].values)
        client.socket.feed(packet(False, 4, 3))
        with self.assertRaises(client_module.sync_wire.SyncError):
            client.ask('advance')
        self.assertEqual(client.replicas['alice'].values, before)
        self.assertEqual(client.replicas['alice'].sequence, 2)
        client.socket.feed(packet(sequence=5))
        self.assertEqual(client.ask('get')['packet']['sequence'], '5')
        client.socket.feed(packet(stream='authenticated-replacement'))
        self.assertEqual(client.ask('login', player='alice', token='test-token')['packet']['sequence'], '1')
        self.assertEqual(client.stream, 'authenticated-replacement')


if __name__ == "__main__":
    unittest.main()
