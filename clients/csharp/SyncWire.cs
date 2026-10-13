using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text;
using System.Text.Json;

namespace Chwell.Sync
{
    public sealed class SyncException : Exception
    {
        public SyncException(string message) : base(message) { }
    }

    // Packets own their decoded values; no input buffer or mutable collection escapes.
    public sealed class WirePacket
    {
        public string Schema { get; }
        public uint SchemaVersion { get; }
        public string Stream { get; }
        public string Entity { get; }
        public ulong Sequence { get; }
        public ulong BaseSequence { get; }
        public bool Snapshot { get; }
        // Values are exactly long, double, bool or string, according to the wire tag.
        public IReadOnlyDictionary<uint, object> Fields { get; }

        internal WirePacket(string schema, uint version, string stream, string entity,
                            ulong sequence, ulong baseline, bool snapshot, Dictionary<uint, object> fields)
        {
            Schema = schema; SchemaVersion = version; Stream = stream; Entity = entity;
            Sequence = sequence; BaseSequence = baseline; Snapshot = snapshot;
            Fields = new ReadOnlyDictionary<uint, object>(fields);
        }
    }

    public static class SyncDecoder
    {
        public const int MaxBytes = 65535;
        internal static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);

        internal static bool Identity(string value)
        {
            if (string.IsNullOrEmpty(value) || value.IndexOf('\0') >= 0) return false;
            try { return Utf8.GetByteCount(value) <= 256; }
            catch (EncoderFallbackException) { return false; }
        }

        private ref struct Reader
        {
            private readonly ReadOnlySpan<byte> bytes;
            private int offset;
            public Reader(ReadOnlySpan<byte> input) { bytes = input; offset = 0; }
            public bool Done => offset == bytes.Length;
            public ReadOnlySpan<byte> Take(int count)
            {
                if (count < 0 || count > bytes.Length - offset) throw new SyncException("Truncated sync packet");
                var part = bytes.Slice(offset, count); offset += count; return part;
            }
            public ulong Integer(int width)
            {
                ulong value = 0;
                foreach (byte part in Take(width)) value = (value << 8) | part;
                return value;
            }
            public string Text(int width, int limit)
            {
                ulong count = Integer(width);
                if (count > (ulong)limit) throw new SyncException("Invalid sync string length");
                try { return Utf8.GetString(Take((int)count)); }
                catch (DecoderFallbackException) { throw new SyncException("Invalid UTF-8"); }
            }
        }

        // Validates structure only. Use SchemaReplica before trusting field values.
        public static WirePacket Decode(ReadOnlySpan<byte> payload, int maxBytes = MaxBytes)
        {
            if (maxBytes < 0 || payload.Length > Math.Min(maxBytes, MaxBytes))
                throw new SyncException("Sync packet exceeds byte limit");
            var reader = new Reader(payload);
            if (!reader.Take(4).SequenceEqual(new byte[] { 67, 72, 87, 83 }))
                throw new SyncException("Invalid sync magic");
            if (reader.Integer(2) != 1) throw new SyncException("Unsupported sync protocol version");
            ulong kind = reader.Integer(1), flags = reader.Integer(1);
            if ((kind != 1 && kind != 2) || flags != 0) throw new SyncException("Invalid packet kind or reserved flags");
            uint version = (uint)reader.Integer(4);
            ulong sequence = reader.Integer(8), baseline = reader.Integer(8);
            string schema = reader.Text(2, 256), stream = reader.Text(2, 256), entity = reader.Text(2, 256);
            if (!Identity(schema) || !Identity(stream) || !Identity(entity) || version == 0 || sequence == 0)
                throw new SyncException("Invalid packet identity, version or sequence");
            if (kind == 1 ? baseline != 0 : baseline == 0 || baseline == ulong.MaxValue || sequence != baseline + 1)
                throw new SyncException("Invalid snapshot/delta baseline");
            ulong count = reader.Integer(2);
            if (count > 1024) throw new SyncException("Too many fields");
            var fields = new Dictionary<uint, object>();
            uint previous = 0;
            for (ulong index = 0; index < count; ++index)
            {
                uint id = (uint)reader.Integer(4);
                if (id <= previous) throw new SyncException("Field IDs must be unique and increasing");
                previous = id;
                object value;
                switch (reader.Integer(1))
                {
                    case 1: value = unchecked((long)reader.Integer(8)); break;
                    case 2:
                        double number = BitConverter.Int64BitsToDouble(unchecked((long)reader.Integer(8)));
                        if (!double.IsFinite(number)) throw new SyncException("Non-finite double");
                        value = number; break;
                    case 3:
                        ulong boolean = reader.Integer(1);
                        if (boolean > 1) throw new SyncException("Invalid bool");
                        value = boolean == 1; break;
                    case 4: value = reader.Text(4, MaxBytes); break;
                    default: throw new SyncException("Unknown field type tag");
                }
                fields.Add(id, value);
            }
            if (!reader.Done) throw new SyncException("Trailing sync bytes");
            return new WirePacket(schema, version, stream, entity, sequence, baseline, kind == 1, fields);
        }
    }

    // Immutable client metadata, read from the generated Contract.SchemaJson.
    public sealed class SchemaContract
    {
        internal sealed class Field
        {
            internal readonly string Kind;
            internal readonly bool Owner;
            private readonly long? intMin, intMax;
            private readonly double? doubleMin, doubleMax;
            private readonly uint maxBytes;

            internal Field(JsonElement json)
            {
                Kind = json.GetProperty("type").GetString() ?? "";
                string visibility = json.GetProperty("visibility").GetString() ?? "";
                if (visibility != "public" && visibility != "owner") throw new SyncException("Invalid client visibility");
                Owner = visibility == "owner";
                switch (Kind)
                {
                    case "int64":
                        if (json.TryGetProperty("min", out var low)) intMin = low.GetInt64();
                        if (json.TryGetProperty("max", out var high)) intMax = high.GetInt64();
                        if (intMin > intMax) throw new SyncException("Invalid integer constraints");
                        break;
                    case "double":
                        if (json.TryGetProperty("min", out var lower)) doubleMin = lower.GetDouble();
                        if (json.TryGetProperty("max", out var upper)) doubleMax = upper.GetDouble();
                        if ((doubleMin.HasValue && !double.IsFinite(doubleMin.Value)) ||
                            (doubleMax.HasValue && !double.IsFinite(doubleMax.Value)) || doubleMin > doubleMax)
                            throw new SyncException("Invalid double constraints");
                        break;
                    case "string":
                        maxBytes = json.TryGetProperty("max_bytes", out var size) ? size.GetUInt32() : SyncDecoder.MaxBytes;
                        break;
                    case "bool": break;
                    default: throw new SyncException("Unknown schema field type");
                }
                if ((Kind != "int64" && Kind != "double" &&
                     (json.TryGetProperty("min", out _) || json.TryGetProperty("max", out _))) ||
                    (Kind != "string" && json.TryGetProperty("max_bytes", out _)))
                    throw new SyncException("Constraint type mismatch");
            }

            internal void Validate(object value)
            {
                bool valid = Kind switch
                {
                    "int64" => value is long integer && !(integer < intMin || integer > intMax),
                    "double" => value is double number && double.IsFinite(number) && !(number < doubleMin || number > doubleMax),
                    "bool" => value is bool,
                    "string" => value is string text && SyncDecoder.Utf8.GetByteCount(text) <= maxBytes,
                    _ => false
                };
                if (!valid) throw new SyncException("Wrong field type or constraint violation");
            }
        }

        public string Name { get; }
        public uint Version { get; }
        public uint KeyFieldId { get; }
        internal readonly IReadOnlyDictionary<uint, Field> Fields;

        private SchemaContract(string name, uint version, uint key, Dictionary<uint, Field> fields)
        {
            Name = name; Version = version; KeyFieldId = key;
            Fields = new ReadOnlyDictionary<uint, Field>(fields);
        }

        private static void UniqueProperties(JsonElement json)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in json.EnumerateObject())
                if (!names.Add(property.Name)) throw new SyncException("Duplicate schema property");
        }

        public static SchemaContract Parse(string schemaJson, uint keyFieldId)
        {
            if (schemaJson == null || schemaJson.Length > 1024 * 1024) throw new SyncException("Invalid schema size");
            try
            {
                using var document = JsonDocument.Parse(schemaJson);
                var root = document.RootElement;
                UniqueProperties(root);
                string name = root.GetProperty("name").GetString() ?? "";
                uint version = root.GetProperty("version").GetUInt32();
                if (!SyncDecoder.Identity(name) || version == 0 || keyFieldId == 0)
                    throw new SyncException("Invalid schema identity or version");
                var definitions = root.GetProperty("fields");
                if (definitions.GetArrayLength() == 0 || definitions.GetArrayLength() > 1024)
                    throw new SyncException("Invalid schema field count");
                var fields = new Dictionary<uint, Field>();
                var names = new HashSet<string>(StringComparer.Ordinal);
                foreach (var json in definitions.EnumerateArray())
                {
                    UniqueProperties(json);
                    uint id = json.GetProperty("id").GetUInt32();
                    string fieldName = json.GetProperty("name").GetString() ?? "";
                    if (id == 0 || !SyncDecoder.Identity(fieldName) || !names.Add(fieldName) || fields.ContainsKey(id))
                        throw new SyncException("Invalid or duplicate schema field");
                    var field = new Field(json);
                    if (id == keyFieldId && fieldName != "id") throw new SyncException("Invalid schema key name");
                    fields.Add(id, field);
                }
                if (!fields.TryGetValue(keyFieldId, out var key) || key.Kind != "string" || key.Owner)
                    throw new SyncException("Schema key must be a public string field");
                return new SchemaContract(name, version, keyFieldId, fields);
            }
            catch (Exception error) when (error is JsonException || error is InvalidOperationException ||
                                          error is KeyNotFoundException || error is FormatException || error is OverflowException)
            { throw new SyncException("Invalid client schema: " + error.Message); }
        }
    }

    public enum ReplicaStatus { Applied, Stale, NeedsSnapshot }

    // One owning thread, schema, entity and viewer. Authenticate before ResetStream.
    public sealed class SchemaReplica
    {
        private readonly SchemaContract contract;
        private readonly string entity;
        private readonly bool owner;
        private string? stream;
        private IReadOnlyDictionary<uint, object> values = EmptyValues();
        public IReadOnlyDictionary<uint, object> Values => values;
        public ulong Sequence { get; private set; }
        public bool Ready { get; private set; }

        private static IReadOnlyDictionary<uint, object> EmptyValues() =>
            new ReadOnlyDictionary<uint, object>(new Dictionary<uint, object>());

        public SchemaReplica(SchemaContract contract, string entity, bool isOwner)
        {
            this.contract = contract ?? throw new ArgumentNullException(nameof(contract));
            if (!SyncDecoder.Identity(entity)) throw new SyncException("Invalid replica entity");
            this.entity = entity; owner = isOwner;
        }

        public void ResetStream(string authenticatedStream)
        {
            if (!SyncDecoder.Identity(authenticatedStream)) throw new SyncException("Invalid authenticated sync stream");
            stream = authenticatedStream; values = EmptyValues(); Sequence = 0; Ready = false;
        }

        // Decode and validate everything before publishing new values or sequence.
        // Invalid input throws; missing baseline/stream and stale packets preserve state.
        public ReplicaStatus Apply(ReadOnlySpan<byte> payload, int maxBytes = SyncDecoder.MaxBytes)
        {
            var wire = SyncDecoder.Decode(payload, maxBytes);
            if (wire.Schema != contract.Name || wire.SchemaVersion != contract.Version || wire.Entity != entity)
                throw new SyncException("Schema version or entity mismatch");
            foreach (var pair in wire.Fields)
            {
                if (!contract.Fields.TryGetValue(pair.Key, out var field) || (field.Owner && !owner))
                    throw new SyncException("Unknown or forbidden field");
                field.Validate(pair.Value);
                if (pair.Key == contract.KeyFieldId && (string)pair.Value != entity)
                    throw new SyncException("Entity key mismatch");
            }
            int expected = 0;
            foreach (var field in contract.Fields.Values) if (!field.Owner || owner) ++expected;
            if (wire.Snapshot && wire.Fields.Count != expected) throw new SyncException("Incomplete snapshot");
            if (stream == null || wire.Stream != stream) return ReplicaStatus.NeedsSnapshot;
            if (wire.Sequence <= Sequence) return ReplicaStatus.Stale;
            if (!wire.Snapshot && (!Ready || wire.BaseSequence != Sequence)) return ReplicaStatus.NeedsSnapshot;
            var pending = wire.Snapshot ? new Dictionary<uint, object>() : new Dictionary<uint, object>(values);
            foreach (var pair in wire.Fields) pending[pair.Key] = pair.Value;
            var published = new ReadOnlyDictionary<uint, object>(pending);
            values = published; Sequence = wire.Sequence; Ready = true;
            return ReplicaStatus.Applied;
        }
    }
}
