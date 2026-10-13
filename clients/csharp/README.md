# C# state synchronization

`Chwell.Sync` is a .NET 8 library for the `CHWS` v1 binary payload. It has no
NuGet dependencies. It decodes typed values and atomically applies snapshots
and deltas against a generated schema. Transport framing, login and encryption
belong to the application. The protocol contract is in
[SYNC_PROTOCOL.md](../../docs/SYNC_PROTOCOL.md).

Generate the client entity, then add this project as a `ProjectReference` in
your .NET application:

```sh
python tools/entity_schema.py examples/cluster_reference/player.schema.json \
  --csharp-output /path/to/app/Player.cs
dotnet add /path/to/app/App.csproj reference clients/csharp/Chwell.Sync.csproj
```

The CMake installation includes these sources under
`share/ChwellCore/clients/csharp`. Copy that directory into your application or
reference its project from the installed location. Generated entity definitions
remain standalone; they do not acquire a dependency on this runtime.

```csharp
using Chwell.Sync;
using Contract = Chwell.Generated.ClusterPlayer.Contract;

var schema = SchemaContract.Parse(Contract.SchemaJson, Contract.KeyFieldId);
var replica = new SchemaReplica(schema, "alice", isOwner: true);
// Only after successful authenticated login, using the established stream:
replica.ResetStream(authenticatedStream);

try
{
    var status = replica.Apply(payloadBytes, maxBytes: 3072);
    if (status == ReplicaStatus.Applied)
    {
        long level = (long)replica.Values[Contract.field_level];
        // Publish validated values to the UI or construct a new ClientEntity.
    }
    else if (status == ReplicaStatus.NeedsSnapshot)
    {
        // Request a current snapshot through the authenticated connection.
    }
}
catch (SyncException)
{
    // Reject malformed, forbidden, incorrectly typed or incompatible data.
}
```

`SyncDecoder.Decode` checks binary structure and returns an immutable packet;
use `SchemaReplica.Apply` to check schema name/version, entity, field visibility,
types and constraints before trusting values. Boxed values are exactly `long`,
`double`, `bool` and `string`; sequences use `ulong`. Integer limits never pass
through floating point. Text uses strict UTF-8; value strings preserve NUL,
while schema/stream/entity identities reject NUL.

`Applied` publishes a new read-only value dictionary and sequence. `Stale`
ignores duplicate/older valid packets. `NeedsSnapshot` means a missing baseline
or unestablished/different stream. Invalid packets throw `SyncException`.
All rejected inputs leave values, sequence and readiness unchanged. Previously
obtained dictionary views remain unchanged after successful updates or reset.

`ResetStream` clears values and baseline. An arbitrary incoming snapshot must
never authenticate or switch streams. On reconnect create a new replica or
reset only after authenticated stream establishment. Owner visibility is an
application decision based on the authenticated viewer, not a flag in a packet.
Use one owning thread per replica; this library does not synchronize concurrent
calls. It does not automatically retry mutations, migrate schema versions or
restore request results. The verified target is .NET 8 on Linux and Windows;
other runtimes, including Unity, require separate integration and verification.

```sh
python tests/test_sync_wire_csharp.py --require-dotnet
```

Tests compile and execute this project with a generated client contract, shared
C++ golden bytes and Python differential cases. They cover truncated payloads,
all field types, exact 64-bit bounds, UTF-8, visibility, gaps, stale packets,
stream reset, snapshot recovery, malformed metadata and unchanged state on
rejection. Installed-package CI runs the same tests using relocated sources.
