# syntax=docker/dockerfile:1

# --- Stage: builder -----------------------------------------------------
# Toolchain completa (GCC, CMake, headers de dev) para compilar e testar.
# Ubuntu 24.04 (não Debian bookworm): precisa de libpqxx-dev >= 7.x
# (bookworm só tem 6.4.5, e o código usa API do libpqxx 7 — pqxx::params/pqxx::prepped).
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        libssl-dev \
        libpqxx-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build -j"$(nproc)"

# --- Stage: runtime ------------------------------------------------------
# Imagem final mínima, só com as libs de runtime e o binário do gateway.
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libpqxx-7.8t64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/src/service_broker/service_gateway_http ./service_gateway_http
COPY --from=builder /src/routes.json ./routes.json

# broker TCP (registro de serviços) + HTTP API
EXPOSE 8080 3001

ENTRYPOINT ["./service_gateway_http"]
