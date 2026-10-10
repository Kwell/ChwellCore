FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make python3 libcurl4-openssl-dev nlohmann-json3-dev default-libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCHWELL_USE_YAML=OFF -DCHWELL_USE_PROTOBUF=OFF \
    -DCHWELL_USE_MYSQL=ON -DCHWELL_USE_CONSUL=ON -DCHWELL_USE_ENTITY_SCHEMA=ON \
    -DCHWELL_BUILD_TESTS=OFF -DCHWELL_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 4 --target cluster_reference
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4t64 libmysqlclient21 \
    && rm -rf /var/lib/apt/lists/* && useradd --system --uid 10001 chwell
COPY --from=build /build/examples/cluster_reference/cluster_reference /usr/local/bin/
USER chwell
ENTRYPOINT ["cluster_reference"]
