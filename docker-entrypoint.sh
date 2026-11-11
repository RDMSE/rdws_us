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

#
# Só vale para o gateway: o 4º argumento dele é o routes.json. Outros serviços podem ter um
# 4º argumento qualquer (ex. sensor_simulator_service: --device-id N --gateway tcp://...),
# por isso o filtro por extensão.
routes_file="$4"
case "$routes_file" in
  *.json)
    if [ ! -f "$routes_file" ] && [ -f ./routes.json ]; then
      mkdir -p "$(dirname "$routes_file")"
      cp ./routes.json "$routes_file"
    fi
    ;;
esac

exec ./service "$@"
