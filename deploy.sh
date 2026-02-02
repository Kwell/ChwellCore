#!/usr/bin/env bash
set -e

IMAGE_NAME="chwellframecore:latest"
CONTAINER_NAME="chwell_server"

# 选择要启动的服务: example_http_server / example_protocol_server
SERVICE_BINARY=${1:-example_http_server}

echo ">>> Building Docker image: $IMAGE_NAME"
docker build -t "$IMAGE_NAME" .

# 如果容器已存在，先停掉并删除
if docker ps -a --format '{{.Names}}' | grep -w "$CONTAINER_NAME" >/dev/null 2>&1; then
  echo ">>> Stopping and removing existing container: $CONTAINER_NAME"
  docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
fi

echo ">>> Running container: $CONTAINER_NAME (service=$SERVICE_BINARY)"
docker run -d \
  --name "$CONTAINER_NAME" \
  -e SERVICE_BINARY="$SERVICE_BINARY" \
  -p 8080:8080 \
  -p 9000:9000 \
  "$IMAGE_NAME"

echo ">>> Done."
echo "  - HTTP:   http://<your-linux-ip>:8080/  (example_http_server)"
echo "  - TCP:    <your-linux-ip>:9000         (example_protocol_server)"

