# syntax=docker/dockerfile:1
#
# One image for every service (gateway + microservices): the runtime stage carries all the
# binaries the builder compiled and tested, and the container picks one with RDWS_SERVICE
# (docker-entrypoint.sh). CI builds and pushes this single image; the deploys pull it.
#   docker build -t rdws_us-services:qa .
#   docker run -e RDWS_SERVICE=auth_service rdws_us-services:qa <args>

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

# Complete build (all targets, including tests): the most expensive layer, cached between
# builds while the sources don't change. The version is deliberately NOT an input here: it is
# the commit's `git describe`, so it would change the cache key on every commit (a merge would
# recompile what its PR's CI had just compiled). The binaries read it at runtime from
# RDWS_VERSION, set in the runtime stage below (rdws::version()); compiled in, it stays the
# placeholder v0.0.0-unknown (.git isn't in the build context).
# The job count shows in the build log: all the CPUs the build sees (no limit on the daemon)
RUN echo "compile jobs: $(nproc)" \
    && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build -j"$(nproc)"

# The service binaries that go into the image. They are located in different places depending
# on the target (the gateway has no fixed RUNTIME_OUTPUT_DIRECTORY, the other services go to
# build/bin/), so they are found by name instead of a fixed path; a missing one fails the build.
ARG SERVICES="service_gateway_http auth_service farm_service field_service device_service \
device_config_service sensor_service sensor_reading_service persistence_service \
ingestion_service reading_writer_service sensor_simulator_service"
RUN mkdir -p /out \
    && for svc in ${SERVICES}; do \
         bin="$(find build -maxdepth 4 -type f -name "$svc" -perm -u+x | head -1)"; \
         [ -n "$bin" ] || { echo "binary not found: $svc" >&2; exit 1; }; \
         cp "$bin" "/out/$svc"; \
       done

# --- Stage: runtime ------------------------------------------------------
# Minimal final image, only with the runtime libs and the service binaries.
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libpqxx-7.8t64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /out/ ./bin/
COPY --from=builder /src/routes.json ./routes.json
COPY docker-entrypoint.sh ./docker-entrypoint.sh
RUN chmod +x ./docker-entrypoint.sh

# TCP broker (service registry) + HTTP API — only the gateway listens, but harmless
# to declare for the other services as well.
EXPOSE 8080 3001

# Last, and cheap: the version of the solution (scripts/rdws_version.sh in CI, README "Versões"),
# read at runtime by rdws::version(). Empty = the compiled placeholder.
ARG RDWS_VERSION=
ENV RDWS_VERSION=${RDWS_VERSION}

ENTRYPOINT ["./docker-entrypoint.sh"]
