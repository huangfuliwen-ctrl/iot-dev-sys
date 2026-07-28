# dev-sys-cloud Docker 镜像 — 多阶段构建
# 用法: docker build -f deploy/Dockerfile -t dev-sys-cloud .

# === 阶段1: 编译 ===
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake g++ libssl-dev libcurl4-openssl-dev libsqlite3-dev libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 编译
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DUSE_SYSTEM_PAHO=OFF \
    -DUSE_SYSTEM_LIBPQ=ON \
    && cmake --build build -j$$(nproc) \
    && strip build/bin/dev-sys-cloud

# === 阶段2: 运行 ===
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 ca-certificates curl libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false devsys \
    && mkdir -p /app/config /app/firmware_files /app/logs /data/dev-sys \
    && chown -R devsys:devsys /app /data/dev-sys

COPY --from=builder /build/build/bin/dev-sys-cloud /usr/local/bin/
COPY config/mqtt_config.json /app/config/
COPY deploy/production/dev-sys.json /app/config/mqtt_config.json

WORKDIR /app
EXPOSE 9080

USER devsys
HEALTHCHECK --interval=30s --timeout=5s CMD curl -sf http://localhost:9080/api/v1/health || exit 1
CMD ["/usr/local/bin/dev-sys-cloud"]
