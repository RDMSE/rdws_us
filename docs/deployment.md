# Dockerização + CI/CD (dev homelab / prod VPS)

## Contexto

Hoje todos os serviços (`auth_service`, `farm_service`, `field_service`, `device_service`,
`device_config_service`, `sensor_service`, `sensor_reading_service`, `persistence_service`)
rodam como binários C++ soltos, iniciados manualmente (ou via `.vscode/launch.json`) e
conectados ao `service_gateway_http` (broker) por socket UNIX/TCP. Não existe Docker,
CI/CD, nem separação de ambientes — só o fluxo local via CMake.

O objetivo é introduzir três cenários sem tocar no fluxo local:

- **local**: continua exatamente como está (CMake + launch.json).
- **dev**: push no GitHub → Actions (runner self-hosted no homelab) builda imagens,
  publica no GHCR e sobe via `docker compose` no próprio homelab.
- **prod**: promoção da mesma imagem (já validada em dev) → deploy via SSH numa VPS
  Digital Ocean de 1GB (2GB depois), rodando tudo (serviços + Postgres + Prometheus +
  Grafana) num único `docker-compose.yml` com limites de memória ajustados.

Motivação: arquitetura já é desacoplada (broker + serviços via socket/TCP), então é o
ponto natural para conteinerizar sem reescrever nada — só empacotar o que já existe.

## Escopo desta primeira fase

Começar simples (docker-compose), sem Swarm/k8s — migração para orquestrador mais
complexo fica para depois, sem jogar fora as imagens/Dockerfiles construídos agora.

**Primeiro passo concreto**: dockerizar só o gateway (`service_gateway_http`), como prova
de conceito — valida o Dockerfile multi-stage (CMake + libpqxx + libssl) e o entrypoint
antes de replicar o padrão pros demais serviços.

## 1. Dockerfiles

Um `Dockerfile` multi-stage por serviço (ou um Dockerfile parametrizado reaproveitado
via `--build-arg SERVICE=auth_service`, a decidir na implementação — mais simples de
manter é um único Dockerfile genérico dado que todos os serviços seguem o mesmo padrão
de build CMake e mesma lista de dependências):

- **Stage build**: imagem com toolchain C++20 + CMake + libpqxx-dev + libssl-dev,
  roda `cmake -S . -B build -DBUILD_TESTING=OFF && cmake --build build --target <service>`.
- **Stage runtime**: imagem slim (ex. `debian:bookworm-slim`) só com libpqxx/libssl
  runtime + o binário copiado de `build/bin/<service>`.
- Entrypoint recebe os args posicionais que os serviços já esperam
  (`<serviceId> <machineName> unix:///tmp/rdws_gateway.sock` ou `--dev`), configuráveis
  via env vars no compose (`SERVICE_ID`, `GATEWAY_ADDRESS`).
- Dockerfile próprio para o broker (`service_gateway_http`), expondo `PORT` (8080) e o
  socket UNIX compartilhado via volume entre os containers.
- Dockerfile para migrations: imagem oficial `flyway/flyway`, montando `db/migrations`
  e `db/flyway.toml`, rodado como job (`docker compose run migrate`) antes de subir os
  serviços.

## 2. docker-compose

- **`docker-compose.dev.yml`** (homelab): broker + todos os serviços + Postgres/PostGIS+
  TimescaleDB + Prometheus + Grafana, rede compartilhada, volume nomeado para o socket
  UNIX do broker, env vars equivalentes ao `.env.dev` atual.
- **`docker-compose.prod.yml`** (VPS): mesma topologia, mas com `deploy.resources.limits`
  de memória por serviço pensando no teto de 1GB total (Postgres com `shared_buffers`
  reduzido, Prometheus com `--storage.tsdb.retention.time` curto, ex. 3-7 dias) — e um
  comentário indicando que os limites sobem quando a VPS for pra 2GB.
- Ambos herdam de um `docker-compose.base.yml` opcional para evitar duplicação (serviços,
  imagens, healthchecks), com overrides só de recursos/replicas/env por ambiente.

## 3. Observabilidade (gap a resolver)

Hoje não existe endpoint `/metrics` em formato Prometheus — só `/health`, `/status`,
`/connections` no broker. Para o Prometheus ter o que raspar:

- Expor um endpoint `/metrics` no `service_gateway_http` (ou em cada serviço) reaproveitando
  o `MetricsTracker` já existente (`src/shared/utils/metrics.h/.cpp`), formatando saída no
  padrão texto do Prometheus (sem precisar de lib nova — é só um formatter).
- `prometheus.yml` com scrape configs apontando pros serviços/broker.
- Dashboard inicial no Grafana provisionado via arquivo (datasource + dashboard JSON
  versionados em `infra/grafana/`), evitando setup manual.

**Logs no Grafana (Loki + Promtail)**: Prometheus só cobre métricas — não indexa texto de
log. Para logs no Grafana, entra o **Loki** (armazenamento/indexação de logs, datasource
nativo do Grafana) alimentado pelo **Promtail** (coleta os logs dos containers Docker,
sem precisar mudar nada no `logger.cpp` — os serviços já escrevem em `stdout` per §3.1).
Decisão já registrada em `PLANO_INGESTION.md` ("Grafana unificando PostgreSQL/métricas e
Loki/logs"); aqui fica formalizado que ambos (Loki + Promtail) entram como serviços de
infraestrutura no compose, mesma categoria de Postgres/Prometheus/Grafana — sem exigir
mudança na aplicação.

Stack de observabilidade completo: **Prometheus** (métricas) + **Loki** (logs) +
**Promtail** (coletor) + **Grafana** (visualização unificada).

**RabbitMQ**: já coberto no `PLANO_INGESTION.md` (fila do pipeline de ingestão) — entra
no mesmo compose como serviço de infraestrutura (imagem oficial, sem build próprio), com
plugin de management/Prometheus exporter habilitado. Não duplicado aqui; ver aquele
documento para detalhes.

## 3.1 Configuração (`.env`) e logs em container

- **`.env`**: o `Config` (`src/shared/config/config.cpp`) só usa `dotenv::init()` como
  fallback para popular variáveis de ambiente a partir de um arquivo — a leitura real é
  sempre via `std::getenv`. Em container isso significa **não** embutir um arquivo `.env`
  na imagem: as variáveis (`DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD`, `DB_NAME`,
  `JWT_SECRET` etc.) são injetadas via `environment:`/`env_file:` no `docker-compose.yml`
  de cada ambiente. Segredos (senha do banco, `JWT_SECRET`) entram via GitHub Secrets nos
  pipelines de deploy, nunca hardcoded na imagem. O `.env.dev` local continua existindo
  só para o fluxo sem Docker.
- **Logs**: o `logger.cpp` já escreve em dois sinks — `stdout` (colorido) e um arquivo
  rotativo `logs/<name>.log` relativo ao cwd. Em container, manter só o sink de `stdout`
  (padrão Docker: `docker logs`, drivers de log, ou Promtail/Loki lendo os logs do
  container) e desabilitar/ignorar o sink de arquivo — evita depender de volume por
  serviço só para não perder logs ao reiniciar o container. Centralização de logs fica
  alinhada com o Grafana já previsto no stack (Loki como fonte adicional, futuramente).

## 4. GitHub Actions

- **Runner self-hosted** registrado no homelab (setup manual, fora do escopo do CI em si).
- **`.github/workflows/build.yml`**: em push/PR — build de cada imagem (matrix por
  serviço), roda `ctest`, e em push na branch principal faz `docker push` pro
  `ghcr.io/rdmeneze/rdws_us/<service>:<sha>` (+ tag `latest` ou `dev`).
- **`.github/workflows/deploy-dev.yml`**: gatilho após build bem-sucedido na branch
  principal — roda no runner self-hosted, faz `docker compose -f docker-compose.dev.yml
  pull && up -d` direto no homelab (mesma máquina do runner ou via SSH local).
- **`.github/workflows/deploy-prod.yml`**: gatilho manual (`workflow_dispatch`) ou por
  tag/release — reusa a mesma imagem já testada em dev (promoção, não rebuild), conecta
  na VPS via SSH (chave em GitHub Secrets) e roda `docker compose -f
  docker-compose.prod.yml pull && up -d`.
- Migrations do Flyway rodam como step/job dedicado antes do `up -d` dos serviços, em
  ambos os pipelines de deploy.

## 5. Documentação

Este documento é o registro das decisões tomadas — os três ambientes, como rodar
localmente (sem mudança), como rodar dev/prod via compose, e como funciona o pipeline
de CI/CD — servindo de referência para a implementação e para sessões futuras.

## Verificação

1. `docker compose -f docker-compose.dev.yml up -d` local (ou no homelab) e confirmar
   via `GET /health` do broker que todos os serviços se registraram.
2. Confirmar que uma chamada autenticada fim-a-fim (login no `auth_service` → CRUD num
   serviço, ex. `farm_service`) funciona através do broker containerizado.
3. Confirmar que o Flyway aplicou as migrations (`flyway_schema_history` populada) e o
   `seed_fake_data.sql` roda sem erro.
4. Verificar `curl localhost:9090/targets` (Prometheus) mostrando todos os alvos "up", e
   o dashboard Grafana carregando dados reais.
5. Rodar o workflow `deploy-dev` manualmente uma vez e conferir logs do runner
   self-hosted; só depois habilitar `deploy-prod` contra a VPS.
