import importlib.util
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "chwell_sync_wire", ROOT / "examples" / "cluster_reference" / "sync_wire.py"
)
sync_wire = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync_wire)


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


if __name__ == "__main__":
    unittest.main()
