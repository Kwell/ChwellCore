using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using Chwell.Sync;
using Contract = Chwell.Generated.ClusterPlayer.Contract;
using Player = Chwell.Generated.ClusterPlayer.ClientEntity;

static class Program
{
    static int checks;
    static void Check(bool value, string message)
    {
        ++checks;
        if (!value) throw new Exception(message);
    }

    static void Reject(Action action)
    {
        try { action(); }
        catch (SyncException) { ++checks; return; }
        throw new Exception("Expected invalid input to fail");
    }

    static void Values(JsonElement expected, IReadOnlyDictionary<uint, object> actual)
    {
        int count = 0;
        foreach (var property in expected.EnumerateObject())
        {
            ++count;
            uint id = uint.Parse(property.Name);
            Check(actual.ContainsKey(id), "Missing field");
            object value = actual[id];
            bool equal = value switch
            {
                long integer => integer == property.Value.GetInt64(),
                double number => BitConverter.DoubleToInt64Bits(number) == BitConverter.DoubleToInt64Bits(property.Value.GetDouble()),
                bool boolean => boolean == property.Value.GetBoolean(),
                string text => text == property.Value.GetString(),
                _ => false
            };
            Check(equal, "Incorrect typed field value");
        }
        Check(count == actual.Count, "Extra field");
    }

    static void Immutable(IReadOnlyDictionary<uint, object> values)
    {
        try { ((IDictionary<uint, object>)values).Add(uint.MaxValue, "changed"); }
        catch (NotSupportedException) { ++checks; return; }
        throw new Exception("Mutable public values");
    }

    static void Main(string[] args)
    {
        using var data = JsonDocument.Parse(File.ReadAllText(args[0]));
        var root = data.RootElement;
        foreach (var sample in root.GetProperty("decode").EnumerateArray())
        {
            byte[] bytes = Convert.FromHexString(sample.GetProperty("hex").GetString()!);
            int limit = sample.GetProperty("limit").GetInt32();
            if (!sample.GetProperty("valid").GetBoolean()) { Reject(() => SyncDecoder.Decode(bytes, limit)); continue; }
            var wire = SyncDecoder.Decode(bytes, limit);
            var expected = sample.GetProperty("expected");
            Check(wire.Schema == expected.GetProperty("schema").GetString(), "Schema");
            Check(wire.SchemaVersion == expected.GetProperty("schema_version").GetUInt32(), "Version");
            Check(wire.Stream == expected.GetProperty("stream").GetString(), "Stream");
            Check(wire.Entity == expected.GetProperty("entity").GetString(), "Entity");
            Check(wire.Sequence == expected.GetProperty("sequence").GetUInt64(), "Sequence precision");
            Check(wire.BaseSequence == expected.GetProperty("base_sequence").GetUInt64(), "Base precision");
            Check(wire.Snapshot == expected.GetProperty("snapshot").GetBoolean(), "Kind");
            Values(expected.GetProperty("fields"), wire.Fields);
            foreach (var tag in expected.GetProperty("types").EnumerateObject())
            {
                object value = wire.Fields[uint.Parse(tag.Name)];
                Check(tag.Value.GetInt32() == (value is long ? 1 : value is double ? 2 : value is bool ? 3 : 4), "Wire type tag");
            }
            Immutable(wire.Fields);
            if (bytes.Length != 0) bytes[0] = 0;
            Values(expected.GetProperty("fields"), wire.Fields); // No retained input buffer.
        }
        foreach (var sample in root.GetProperty("replicas").EnumerateArray())
        {
            var metadata = SchemaContract.Parse(sample.GetProperty("schema_json").GetString()!, sample.GetProperty("key").GetUInt32());
            var replica = new SchemaReplica(metadata, sample.GetProperty("entity").GetString()!, sample.GetProperty("owner").GetBoolean());
            foreach (var step in sample.GetProperty("steps").EnumerateArray())
            {
                var before = replica.Values;
                ulong sequence = replica.Sequence;
                bool ready = replica.Ready;
                string status = step.GetProperty("status").GetString()!;
                if (step.TryGetProperty("reset", out var reset))
                {
                    if (status == "invalid") Reject(() => replica.ResetStream(reset.GetString()!));
                    else replica.ResetStream(reset.GetString()!);
                }
                else
                {
                    var bytes = Convert.FromHexString(step.GetProperty("hex").GetString()!);
                    if (status == "invalid") Reject(() => replica.Apply(bytes));
                    else Check(replica.Apply(bytes).ToString() == status, "Replica outcome " + status);
                }
                if (status == "invalid" || status == "Stale" || status == "NeedsSnapshot")
                    Check(ReferenceEquals(before, replica.Values) && sequence == replica.Sequence && ready == replica.Ready,
                          "Rejected input changed replica");
                Values(step.GetProperty("values"), replica.Values);
                Check(replica.Sequence == step.GetProperty("sequence").GetUInt64(), "Replica sequence");
                Check(replica.Ready == step.GetProperty("ready").GetBoolean(), "Replica ready");
                Immutable(replica.Values);
            }
        }
        foreach (var sample in root.GetProperty("invalid_schemas").EnumerateArray())
            Reject(() => SchemaContract.Parse(sample.GetProperty("json").GetString()!, sample.GetProperty("key").GetUInt32()));

        // Consume the real generated C# contract and project only validated values.
        var contract = SchemaContract.Parse(Contract.SchemaJson, Contract.KeyFieldId);
        var playerReplica = new SchemaReplica(contract, "alice", true);
        playerReplica.ResetStream("gateway:1:7");
        Check(playerReplica.Apply(Convert.FromHexString(root.GetProperty("cluster_snapshot").GetString()!)) == ReplicaStatus.Applied,
              "Generated contract rejects reference snapshot");
        var player = new Player {
            value_id = (string)playerReplica.Values[Contract.field_id],
            value_level = (long)playerReplica.Values[Contract.field_level],
            value_gold = (long)playerReplica.Values[Contract.field_gold]
        };
        Check(player.value_id == "alice" && player.value_level == 1 && player.value_gold == 3, "Typed projection");
        Check(typeof(Player).GetProperties().Length == 3, "Server fields in generated client");
        Check(!Contract.SchemaJson.Contains("server-only"), "Server default leaked");
        var held = playerReplica.Values;
        playerReplica.ResetStream("replacement");
        Check(held.Count == 3 && playerReplica.Values.Count == 0, "Old read-only view was mutated");
        Reject(() => playerReplica.ResetStream("bad\ud800stream"));
        Check(playerReplica.Sequence == 0 && !playerReplica.Ready, "Invalid reset changed state");
        Console.WriteLine($"PASS: compiled C# sync decoder, generated schema and atomic replica ({checks} checks)");
    }
}
