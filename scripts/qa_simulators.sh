#!/usr/bin/env bash
# (Re)creates one SensorSimulatorService container per simulated device in QA
# (devices.is_simulated = true in rdws_qa), from an image built from the current checkout.
# Runs at the end of deploy-qa.yml, or by hand on the homelab:
#
#   scripts/qa_simulators.sh [env-file]        # default: .env.qa
#
# - Devices come from the database: creating a device with is_simulated = true (the flag is
#   immutable, V7) or deleting one is enough for the next run to start/stop its container.
#   Nothing to edit here or in the compose files.
# - Control port per device: 9100 + id (9109 for device 9), the same one the Bruno
#   collection computes ("91" + id with 2 digits) for ids below 100.
# - Each device keeps its unsent-readings buffer in its own volume (rdws_qa_sim_data_NN), so
#   recreating the container doesn't lose readings (Plano_SensorSimulatorService.md).
# - The containers aren't in docker-compose.qa-app.yml, so they carry their own labels for
#   promtail (infra/promtail/promtail-qa.yml) to ship their logs to Loki.
set -euo pipefail

ENV_FILE="${1:-.env.qa}"
IMAGE="rdws_us-sensor-simulator:qa"
NETWORK="rdws_qa_default"
DB_CONTAINER="rdws_postgres_qa"
NAME_PREFIX="rdws_sim_device_"

if [ ! -f "$ENV_FILE" ]; then
  echo "env file not found: $ENV_FILE" >&2
  exit 1
fi
set -a
# shellcheck disable=SC1090
. "$ENV_FILE"
set +a
: "${DB_USER:=rdws_user}" "${DB_NAME:=rdws_qa}" "${DB_PASSWORD:?DB_PASSWORD not set in $ENV_FILE}"

mapfile -t ids < <(docker exec "$DB_CONTAINER" psql -U "$DB_USER" -d "$DB_NAME" -tAc \
  "SELECT id FROM devices WHERE is_simulated ORDER BY id")
echo "simulated devices: ${ids[*]:-none}"

docker build --target runtime --build-arg SERVICE=sensor_simulator_service -t "$IMAGE" .

container_name() { printf '%s%02d_qa' "$NAME_PREFIX" "$1"; }

# Containers of devices that are no longer simulated, plus the hand-made ones from before
# this script (sim-device-NN).
wanted=" "
for id in "${ids[@]}"; do
  wanted+="$(container_name "$id") "
done
for name in $(docker ps -a --format '{{.Names}}' | grep -E "^(${NAME_PREFIX}[0-9]+_qa|sim-device-[0-9]+)$" || true); do
  if [[ "$wanted" != *" $name "* ]]; then
    echo "removing $name"
    docker rm -f "$name" > /dev/null
  fi
done

for id in "${ids[@]}"; do
  name="$(container_name "$id")"
  port=$((9100 + id))
  echo "starting $name (device $id, control port $port)"
  docker rm -f "$name" > /dev/null 2>&1 || true
  docker run -d --name "$name" \
    --network "$NETWORK" \
    --restart unless-stopped \
    --label rdws.env=qa \
    --label rdws.service=sensor_simulator \
    -p "$port:$port" \
    -v "rdws_qa_sim_data_$(printf '%02d' "$id"):/app/sim_data" \
    -e DB_HOST=postgres -e DB_PORT=5432 \
    -e DB_USER="$DB_USER" -e DB_PASSWORD="$DB_PASSWORD" -e DB_NAME="$DB_NAME" \
    -e RDWS_ENVIRONMENT=qa \
    -e INGESTION_HOST=ingestion_service -e INGESTION_PORT=5684 \
    -e SIMULATOR_DATA_DIR=/app/sim_data \
    -e SIMULATOR_CONTROL_PORT="$port" \
    "$IMAGE" --device-id "$id" --gateway tcp://gateway:8080 > /dev/null
done
