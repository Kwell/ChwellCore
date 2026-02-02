#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_DIR="$ROOT_DIR/proto"
OUT_DIR="$ROOT_DIR/generated/proto"

if ! command -v protoc >/dev/null 2>&1; then
  echo "protoc not found. Please install protobuf compiler first."
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "Generating C++ protobuf code into $OUT_DIR"
protoc -I "$PROTO_DIR" --cpp_out="$OUT_DIR" "$PROTO_DIR"/game.proto

echo "Done. Generated files:"
ls "$OUT_DIR"

