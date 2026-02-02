ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE} AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        apt-utils build-essential cmake git ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 拷贝源码到构建镜像中
COPY . /app

# 配置并编译
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . --config Release


ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 从构建阶段拷贝需要的可执行文件
COPY --from=build /app/build/example_http_server /usr/local/bin/example_http_server
COPY --from=build /app/build/example_protocol_server /usr/local/bin/example_protocol_server

# 暴露端口（按需调整）
EXPOSE 8080 9000

# 通过环境变量选择要启动的服务，默认 http server
ENV SERVICE_BINARY=example_http_server

CMD ["/bin/sh", "-c", "/usr/local/bin/$SERVICE_BINARY"]

