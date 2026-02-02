#!/usr/bin/env bash
set -e

# =============================================================================
# 可配置参数（缺省默认值，可通过环境变量覆盖）
# =============================================================================
# IMAGE_NAME        - Docker 镜像名称，缺省: chwellframecore:latest
# CONTAINER_NAME   - 容器名称，缺省: chwell_server
# SERVICE_BINARY   - 要启动的服务: example_http_server / example_protocol_server / example_gateway_server，缺省: example_http_server
# HTTP_PORT        - HTTP 服务端口映射（宿主机:容器），缺省: 8080
# TCP_PORT         - TCP 服务端口映射（宿主机:容器），缺省: 9000
# GATEWAY_PORT     - 网关服务端口映射（宿主机:容器），缺省: 9001
# BACKEND_HOST     - 网关连接的后端逻辑服地址（仅 gateway 需设置），缺省: 127.0.0.1
# BACKEND_PORT     - 后端逻辑服端口，缺省: 9000
# FORCE_BUILD      - 是否强制重新构建镜像（1=是，0=否），缺省: 0（仅当镜像不存在时构建）
# BASE_IMAGE       - 基础镜像，缺省: DaoCloud 镜像（避免 Docker Hub 拉取超时）
#                    使用官方源可设: BASE_IMAGE=ubuntu:22.04
# =============================================================================

IMAGE_NAME="${IMAGE_NAME:-chwellframecore:latest}"
BASE_IMAGE="${BASE_IMAGE:-docker.m.daocloud.io/library/ubuntu:22.04}"
CONTAINER_NAME="${CONTAINER_NAME:-chwell_server}"
SERVICE_BINARY="${SERVICE_BINARY:-example_http_server}"
HTTP_PORT="${HTTP_PORT:-8080}"
TCP_PORT="${TCP_PORT:-9000}"
GATEWAY_PORT="${GATEWAY_PORT:-9001}"
BACKEND_HOST="${BACKEND_HOST:-127.0.0.1}"
BACKEND_PORT="${BACKEND_PORT:-9000}"
FORCE_BUILD="${FORCE_BUILD:-0}"

# 解析参数：SERVICE_BINARY（可选）、--build/-b（强制重新构建）
for arg in "$@"; do
  case "$arg" in
    --build|-b)
      FORCE_BUILD=1
      ;;
    --help|-h)
      echo "Usage: $0 [example_http_server|example_protocol_server|example_gateway_server] [--build|-b]"
      echo ""
      echo "Options:"
      echo "  --build, -b  强制重新构建镜像（缺省：仅无镜像时构建）"
      echo ""
      echo "Examples:"
      echo "  $0                              # 使用默认 example_http_server"
      echo "  $0 example_protocol_server      # 启动 protocol 逻辑服"
      echo "  $0 example_gateway_server      # 启动 gateway 网关（需先启动 protocol_server）"
      echo "  $0 --build                      # 强制重新构建后部署"
      echo "  BACKEND_HOST=host.docker.internal $0 example_gateway_server  # 网关连接宿主机后端"
      exit 0
      ;;
    example_http_server|example_protocol_server|example_gateway_server)
      SERVICE_BINARY="$arg"
      ;;
    *)
      echo ">>> Error: unknown argument '$arg'"
      echo ">>> Usage: $0 [example_http_server|example_protocol_server|example_gateway_server] [--build|-b]"
      exit 1
      ;;
  esac
done

# 检查 Docker 是否可用
if ! command -v docker &>/dev/null; then
  echo ">>> Error: docker command not found. Please install Docker first."
  echo "    See: https://docs.docker.com/get-docker/"
  exit 1
fi

# 检查镜像是否存在
image_exists() {
  docker image inspect "$IMAGE_NAME" >/dev/null 2>&1
}

# 无镜像或强制构建时，先构建镜像（启用 BuildKit，支持 BASE_IMAGE 镜像源）
if [[ "$FORCE_BUILD" == "1" ]] || ! image_exists; then
  echo ">>> Building Docker image: $IMAGE_NAME (base=$BASE_IMAGE)"
  docker build --build-arg BASE_IMAGE="$BASE_IMAGE" -t "$IMAGE_NAME" .
else
  echo ">>> Image exists, skip build: $IMAGE_NAME (use --build to force rebuild)"
fi

# 如果容器已存在，先停掉并删除
if docker ps -a --format '{{.Names}}' | grep -w "$CONTAINER_NAME" >/dev/null 2>&1; then
  echo ">>> Stopping and removing existing container: $CONTAINER_NAME"
  docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
fi

# 根据服务类型选择容器名和端口映射
if [[ "$SERVICE_BINARY" == "example_gateway_server" ]]; then
  [[ "$CONTAINER_NAME" == "chwell_server" ]] && CONTAINER_NAME="chwell_gateway"
  DOCKER_PORTS="-p ${GATEWAY_PORT}:9001"
  DOCKER_ENV="-e GATEWAY_PORT=9001 -e BACKEND_HOST=${BACKEND_HOST} -e BACKEND_PORT=${BACKEND_PORT}"
else
  DOCKER_PORTS="-p ${HTTP_PORT}:8080 -p ${TCP_PORT}:9000"
  DOCKER_ENV=""
fi

echo ">>> Running container: $CONTAINER_NAME (service=$SERVICE_BINARY)"
docker run -d \
  --name "$CONTAINER_NAME" \
  -e SERVICE_BINARY="$SERVICE_BINARY" \
  $DOCKER_ENV \
  $DOCKER_PORTS \
  "$IMAGE_NAME"

# 检查容器是否在运行
sleep 1
if ! docker ps --format '{{.Names}}' | grep -w "$CONTAINER_NAME" >/dev/null 2>&1; then
  echo ">>> Error: Container exited immediately. Logs:"
  docker logs "$CONTAINER_NAME" 2>&1
  exit 1
fi

# 获取本机 IP（缺省回退到 localhost）
HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "127.0.0.1")
echo ">>> Done."
if [[ "$SERVICE_BINARY" == "example_gateway_server" ]]; then
  echo "  - Gateway: ${HOST_IP}:${GATEWAY_PORT}  (backend: ${BACKEND_HOST}:${BACKEND_PORT})"
else
  echo "  - HTTP:   http://${HOST_IP}:${HTTP_PORT}/  (example_http_server)"
  echo "  - TCP:    ${HOST_IP}:${TCP_PORT}         (example_protocol_server)"
fi
