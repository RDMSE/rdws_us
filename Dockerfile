# syntax=docker/dockerfile:1
#
# Generic Dockerfile, reused by all services (gateway + microservices)
# via --build-arg SERVICE=<nome-do-target-cmake>. Default: service_gateway_http.
# Examples:
#   docker build --build-arg SERVICE=service_gateway_http -t rdws_us-gateway:qa .
#   docker build --build-arg SERVICE=auth_service         -t rdws_us-auth:qa .

# --- Stage: builder -----------------------------------------------------
# Complete toolchain (GCC, CMake, development headers) for compiling and testing.
# Ubuntu 24.04 (not Debian bookworm): requires libpqxx-dev >= 7.x
# (bookworm only has 6.4.5, and the code uses libpqxx 7 API — pqxx::params/pqxx::prepped).
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

# Complete build (all targets, including tests) — does not reference SERVICE on purpose,
# so this layer (the most expensive one) is cached between builds of different services.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build -j"$(nproc)"

ARG SERVICE=service_gateway_http

# Binary files are located in different places depending on the target (gateway does not have
# a fixed RUNTIME_OUTPUT_DIRECTORY, the other services go to build/bin/) — we locate
# them by name instead of fixing the path.
RUN mkdir -p /out \
    && cp "$(find build -maxdepth 4 -type f -name "${SERVICE}" -perm -u+x | head -1)" /out/service

# --- Stage: runtime ------------------------------------------------------
# Minimal final image, only with the runtime libs and the service binary.
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libpqxx-7.8t64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /out/service ./service
COPY --from=builder /src/routes.json ./routes.json

# TCP broker (service registry) + HTTP API — only the gateway listens, but harmless
# to declare for the other services as well.
EXPOSE 8080 3001

ENTRYPOINT ["./service"]
