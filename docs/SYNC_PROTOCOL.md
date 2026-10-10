# Stable Client Synchronization Protocol

ChwellCore's schema synchronization payload is a small, versioned binary
format carried by an already authenticated and framed transport. It does not
provide encryption, replay protection, or transport framing.

## Packet contract

Every payload starts with ASCII `CHWS`, followed by network byte order fields:

| Field | Size | Meaning |
| --- | ---: | --- |
| wire version | 2 | `1` for this contract |
| kind | 1 | `1` snapshot, `2` delta |
| flags | 1 | reserved, must be zero |
| schema version | 4 | generated entity schema version |
| sequence | 8 | unsigned viewer/entity/stream sequence |
| base sequence | 8 | `0` for snapshots; delta base, with `sequence = base + 1` |
| schema, stream, entity | 2 + bytes each | UTF-8 strings, each at most 256 bytes |
| field count | 2 | at most 1024 |

Fields are sorted by strictly increasing numeric field ID. Each field carries
a four-byte ID, a type tag, and a typed value. Tags are `1` for signed 64-bit
integer, `2` for IEEE-754 binary64, `3` for boolean, and `4` for UTF-8 string.
Strings carry a four-byte byte length and may contain NUL. Non-finite doubles,
malformed UTF-8, unknown fields, wrong types, and fields outside the viewer's
visibility are rejected.

Snapshots contain every field visible to that viewer. Deltas contain only
changed fields. A receiver replaces its cache for a snapshot and atomically
merges a delta. A gap, missing baseline, schema/entity mismatch, or stream
mismatch never partially changes the cache; the receiver requests a new
authenticated snapshot.

The maximum payload is 65,535 bytes. The reference service uses a 3,072-byte
application limit and an 8,192-byte outer request limit.

## Stream and authentication rules

`stream` identifies the authenticated gateway connection and session lease.
The client may call `reset_stream` only after successful authenticated login.
An arbitrary packet cannot switch a replica to a new stream. On reconnect, the
client creates a fresh replica and accepts the login snapshot as sequence 1.

Sequences are per viewer/entity/stream and are represented as decimal strings
in the reference JSON envelope so JavaScript clients do not lose 64-bit
precision. The binary payload always carries the full 64-bit value.

## Reference implementation

The C++ encoder/decoder is in `include/chwell/sync/sync_wire.h` and
`src/sync/sync_wire.cpp`. The standard-library Python decoder and atomic client
replica are in `examples/cluster_reference/sync_wire.py`. The reference service
returns an `encoding=chwell-sync-v1-hex` JSON envelope. The hex wrapper is only
a convenience; production transports should carry the binary payload directly.
