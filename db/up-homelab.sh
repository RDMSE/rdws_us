#!/usr/bin/env bash
# db/up-homelab.sh — start Postgres (dev + qa) on homelab and run Flyway migrations.
# Run from the root of the repo, on the homelab (fedora-server):
#   ./db/up-homelab.sh
#
# Prerequisite: .env.dev-db and .env.qa filled (copy from .env.dev-db.example and
# .env.qa.example the first time — the script creates from the template if they don't exist,
# but with placeholder password; edit before running in real production/QA).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

ensure_env_file() {
    local env_file="$1"
    local example_file="$2"
    if [[ ! -f "$env_file" ]]; then
        echo "==> $env_file does not exist — copying from $example_file (edit the passwords afterwards!)"
        cp "$example_file" "$env_file"
    fi
}

ensure_env_file ".env.dev-db" ".env.dev-db.example"
ensure_env_file ".env.qa" ".env.qa.example"

echo "==> Starting Postgres dev (docker-compose.dev-db.yml)..."
docker compose -f docker-compose.dev-db.yml --env-file .env.dev-db up -d postgres

echo "==> Starting Postgres qa (docker-compose.qa-db.yml)..."
docker compose -f docker-compose.qa-db.yml --env-file .env.qa up -d postgres

echo "==> Waiting for healthcheck..."
for name in rdws_postgres_dev rdws_postgres_qa; do
    for _ in $(seq 1 30); do
        status="$(docker inspect --format='{{.State.Health.Status}}' "$name" 2>/dev/null || echo "starting")"
        [[ "$status" == "healthy" ]] && break
        sleep 1
    done
    echo "    $name: $status"
done

echo "==> Running Flyway migrations (dev)..."
docker compose -f docker-compose.dev-db.yml --env-file .env.dev-db --profile migrate run --rm migrate

echo "==> Running Flyway migrations (qa)..."
docker compose -f docker-compose.qa-db.yml --env-file .env.qa --profile migrate run --rm migrate

echo ""
echo "==> Done. Status of the containers:"
docker ps --filter "name=rdws_postgres" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
