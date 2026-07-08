#!/bin/sh
# Entrypoint genérico (gateway + serviços). Bug real encontrado em produção
# (2026-07-09): o gateway roda com routesFile apontando pra dentro de um volume
# (docker-compose.qa-app.yml: /app/data/routes.json), que começa vazio num volume
# novo — sem isso, o EventRouter carrega zero regras e todo path REST (POST
# /auth/login, GET /farms, ...) responde 404 "No route found", mesmo com
# routes.json (as regras corretas) já embutido na imagem em ./routes.json.
#
# Semeia o volume com o routes.json da imagem só se ainda não existir — depois da
# primeira execução, CRUD de rotas em runtime (POST/PUT/DELETE /routes) persiste
# no volume normalmente entre redeploys, sem esse script sobrescrever nada.
set -e

routes_file="$4"
if [ -n "$routes_file" ] && [ ! -f "$routes_file" ] && [ -f ./routes.json ]; then
  mkdir -p "$(dirname "$routes_file")"
  cp ./routes.json "$routes_file"
fi

exec ./service "$@"
