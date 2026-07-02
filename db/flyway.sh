#!/usr/bin/env bash
# db/flyway.sh — lê .env.dev e invoca Flyway CLI
# Uso: ./db/flyway.sh [migrate|info|validate|repair|clean]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$(dirname "$SCRIPT_DIR")/.env.dev"

if [[ -f "$ENV_FILE" ]]; then
    set -o allexport
    source "$ENV_FILE"
    set +o allexport
fi

flyway \
    -url="jdbc:postgresql://${DB_HOST:-fedora-server.local}:${DB_PORT:-5432}/${DB_NAME:-rdws_us_dev}" \
    -user="${DB_USER:-rdws_user}" \
    -password="${DB_PASSWORD:-rdws_password}" \
    -locations="filesystem:${SCRIPT_DIR}/migrations" \
    -baselineOnMigrate=true \
    "${1:-info}"