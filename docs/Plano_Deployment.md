# Dockerização + CI/CD (dev local / QA homelab / prod VPS)

## Contexto

Hoje todos os serviços (`auth_service`, `farm_service`, `field_service`, `device_service`,
`device_config_service`, `sensor_service`, `sensor_reading_service`, `persistence_service`)
rodam como binários C++ soltos, iniciados manualmente (ou via `.vscode/launch.json`) e
conectados ao `service_gateway_http` (broker) por socket UNIX/TCP. Não existe Docker,
CI/CD, nem separação de ambientes — só o fluxo local via CMake.

O objetivo é introduzir três ambientes sem tocar no fluxo local:

- **dev**: este notebook — continua exatamente como está (CMake + launch.json), sem
  Docker, apontando pro Postgres remoto do homelab (`fedora-server` via Tailscale) num
  banco próprio (`rdws_dev`).
- **QA**: o homelab — push no GitHub → Actions (runner self-hosted no próprio homelab)
  builda imagens, publica no GHCR e sobe via `docker compose` ali, contra o banco
  `rdws_qa`. É o ambiente de integração contínua, sempre no ar.
- **prod**: promoção da mesma imagem (já validada em QA) → deploy via SSH numa VPS
  Digital Ocean de 1GB (2GB depois), rodando tudo (serviços + Postgres + Prometheus +
  Grafana) num único `docker-compose.yml` com limites de memória ajustados, banco
  `rdws_prod`.

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
- Entrypoint recebe os args posicionais que os serviços já esperam, mas em container usa
  a forma **TCP** (`<serviceId> <machineName> tcp://gateway:3001`) em vez de socket UNIX —
  ver justificativa na seção 2. Endereço configurável via env var no compose
  (`SERVICE_ID`, `GATEWAY_ADDRESS`).
- Dockerfile próprio para o broker (`service_gateway_http`), expondo `PORT` (8080, HTTP)
  e a porta TCP do gateway (ex. 3001) usada pelos serviços para se registrar.
- Dockerfile para migrations: imagem oficial `flyway/flyway`, montando `db/migrations`
  e `db/flyway.toml`, rodado como job (`docker compose run migrate`) antes de subir os
  serviços.
- `IngestionService` e `ReadingWriterService` (ver `Plano_Ingestion.md`) seguem o mesmo
  padrão genérico de Dockerfile dos demais serviços. **`SensorSimulatorService` fica de
  fora** — será uma aplicação completamente separada (no máximo com conexão direta ao
  banco), sem entrar no compose principal; vale um plano próprio para ele mais adiante.

## 2. docker-compose

- **`docker-compose.qa-db.yml`** + **`docker-compose.qa-app.yml`** (homelab): banco
  (Postgres/PostGIS) e app (broker + demais serviços, Prometheus, Grafana) em arquivos
  separados, mesmo projeto/rede Compose (`name: rdws_qa` em ambos) — ciclo de vida
  independente, redeploy do app (`pull && up -d` no app) nunca reinicia/derruba o banco
  por engano. Env vars apontando pro banco `rdws_qa`. Ambiente dev (notebook) não usa
  compose para o app — continua no fluxo CMake local; só o banco (`rdws_dev`) é
  containerizado (`docker-compose.dev-db.yml`), separado do de QA.
- **Broker ↔ serviços via TCP, não socket UNIX**: o socket UNIX só funcionaria entre
  containers via volume compartilhado — frágil (single-host, single-instância do broker,
  quebra em k8s/múltiplos hosts). O broker já suporta conexão via `tcp://host:port` além
  de `unix://` (nenhuma mudança de código necessária); em `docker-compose` os nomes dos
  serviços já funcionam como hostname DNS interno (ex: `tcp://gateway:3001`), o que
  também deixa o caminho pronto para uma futura migração a k8s. Socket UNIX continua
  sendo o padrão fora de container (uso em dev local sem Docker).
- **`docker-compose.prod.yml`** (VPS): mesma topologia, mas com `deploy.resources.limits`
  de memória por serviço pensando no teto de 1GB total (Postgres com `shared_buffers`
  reduzido, Prometheus com `--storage.tsdb.retention.time` curto, ex. 3-7 dias) — e um
  comentário indicando que os limites sobem quando a VPS for pra 2GB. Banco `rdws_prod`.
- Ambos herdam de um `docker-compose.base.yml` opcional para evitar duplicação (serviços,
  imagens, healthchecks), com overrides só de recursos/replicas/env por ambiente.

## 2.1 Banco de dados — três instâncias Postgres, todas containerizadas

Homelab reformatado (ambiente limpo) e acessado agora via **Tailscale** (MagicDNS) —
o hostname continua `fedora-server`, só que resolvido pela rede Tailscale em vez do
`.local` (mDNS) anterior. Isso substitui `DB_HOST=fedora-server` por
`DB_HOST=fedora-server` (ou o IP Tailscale) em toda configuração, e também simplifica o
acesso do notebook/CI ao homelab (mesma rede virtual, sem depender de estar na LAN física).

- **`rdws_dev`**: instância própria Postgres containerizada no homelab, usada pelo
  ambiente dev (notebook, fluxo local sem Docker) via Tailscale.
- **`rdws_qa`**: instância própria Postgres containerizada no homelab, em
  `docker-compose.qa-db.yml` (mesma máquina do runner self-hosted, mas container isolado
  do de `rdws_dev` — cada um com seu próprio volume de dados e projeto Compose distinto).
- **`rdws_prod`**: instância própria Postgres containerizada na VPS Digital Ocean,
  isolada das demais.

Três containers Postgres distintos (não instância única com múltiplos databases) — mais
simples de isolar recurso/backup por ambiente, ao custo de rodar 2 instâncias no mesmo
homelab (aceitável, já que o homelab não tem o teto de memória apertado da VPS de 1GB).
- Migrations (Flyway) rodam uma vez por banco/ambiente — mesmo `db/migrations/`, `.toml`
  com env vars diferentes por ambiente (`DB_NAME=rdws_dev|rdws_qa|rdws_prod`).

## 3. Observabilidade ✅

- ✅ **`GET /metrics/prometheus`** no `service_gateway_http`
  (`src/service_broker/HttpGateway.cpp`) — reaproveita o `MetricsTracker` já existente
  (mesmos dados de `GET /metrics` em JSON) e o resumo de conexões/serviços de `GET
  /status`, formatados no padrão texto de exposição do Prometheus (`# HELP`/`# TYPE` +
  séries `rdws_gateway_requests_total`, `_errors_total`, `_timeouts_total`,
  `_latency_avg_ms`, `_latency_p99_ms` por capability, mais
  `rdws_gateway_active_connections`, `_pending_requests`, `_services_total`,
  `_services_healthy`) — sem lib nova, só um formatter (`formatPrometheusMetrics`,
  anonymous namespace no topo do arquivo). Endpoint público (mesmo padrão de
  `/status`/`/health`/`/connections` — nunca passa por `AuthMiddleware`).
- `prometheus-qa.yml`/`prometheus-dev.yml` com scrape config apontando pro gateway.
- Dashboard inicial no Grafana provisionado via arquivo (datasource + dashboard JSON
  versionados em `infra/grafana/`), evitando setup manual — ver detalhes abaixo.

### Dois stacks de observabilidade — dev e qa, separados

**Decisão de design**: um stack de observabilidade por ambiente
(`docker-compose.qa-observability.yml` / `docker-compose.dev-observability.yml`), não um
único compartilhado — QA nunca divide painel/dado com dev, e a forma de coletar métricas
e logs é fundamentalmente diferente entre os dois:

- **QA** (`name: rdws_qa`, mesma rede/projeto dos demais compose de QA): Prometheus
  escrapa `gateway:3001/metrics/prometheus` via DNS interno do Compose; Promtail usa
  Docker service discovery (`docker_sd_configs` + filtro por label
  `com.docker.compose.project=rdws_qa`) pra coletar `stdout` de todos os containers do
  projeto — sem mudança nenhuma no `logger.cpp`.
- **Dev** (`name: rdws_dev_obs`, projeto isolado): o gateway/serviços de dev rodam
  **nativos** (fora do Docker, via CMake) na mesma máquina onde esse compose roda — não
  há container pra escrapar via DNS interno nem via Docker SD. Prometheus usa
  `extra_hosts: host.docker.internal:host-gateway` pra alcançar o processo no host;
  Promtail lê o arquivo de log rotativo (`logs/*.log`, já escrito pelo `logger.cpp`)
  montado read-only, em vez de Docker SD.
- Portas diferentes por ambiente pra rodar os dois no mesmo host sem conflito: QA
  9090 (Prometheus) / 3100 (Loki) / 3300 (Grafana); dev 9091 / 3101 / 3301.
- **Ciclo de vida diferente por ambiente**: QA roda na homelab, sempre no ar
  (`restart: unless-stopped`, igual aos demais compose de QA). Dev roda na máquina do
  desenvolvedor e é **sob demanda** — sem `restart:` no
  `docker-compose.dev-observability.yml` — sobe só quando for investigar algo
  (`up -d`) e derruba depois (`down`); não faz sentido Prometheus/Loki/Grafana
  consumindo recursos da máquina de dev o tempo todo, nem voltando sozinhos a cada
  reinício do Docker Desktop.
- Grafana provisionado 100% via arquivo (`infra/grafana/provisioning/`) — datasources
  (`Prometheus`/`Loki`, UIDs fixos `rdws-prometheus`/`rdws-loki`) e um dashboard inicial
  (`gateway-overview.json`: painéis de serviços saudáveis, conexões ativas, requests/erros
  por capability, latência média, e um painel de logs via Loki) — zero setup manual na UI,
  os dois stacks reutilizam a mesma provisioning porque os nomes/UIDs dos datasources são
  idênticos nos dois `docker-compose.*-observability.yml`.

**Indicador de status por serviço no Grafana**: assim que o Prometheus faz scrape de um
alvo, ele gera automaticamente a métrica `up{job="..."}` (1 = respondendo, 0 = fora do
ar) para cada serviço/broker — sem esforço extra além do `prometheus.yml` já previsto.
Um painel *Stat* com essa métrica já dá um indicador verde/vermelho por serviço no
dashboard. Opcionalmente, dá pra enriquecer isso expondo no `/metrics` alguns dos dados
que o `MetricsTracker`/`ServiceMonitor` já calculam (serviços conectados, capabilities
registradas, taxa de erro) como gauges, indo além do simples "up/down".

**Nota**: com esse indicador via Prometheus + o Bruno cobrindo `/health` e `/status`
(coleção `bruno/IoT Sensor API/Gateway/Health.bru` e `Status.bru`), o
`service_gateway_monitor` (CLI interativo de debug) fica dispensável nos ambientes
containerizados — não é candidato a dockerização (depende de terminal interativo, portas
hardcoded) nem necessário, já que os mesmos dados ficam disponíveis via HTTP/Grafana.

**Logs no Grafana (Loki + Promtail)**: Prometheus só cobre métricas — não indexa texto de
log. Para logs no Grafana, entra o **Loki** (armazenamento/indexação de logs, datasource
nativo do Grafana) alimentado pelo **Promtail** (coleta os logs dos containers Docker,
sem precisar mudar nada no `logger.cpp` — os serviços já escrevem em `stdout` per §3.1).
Decisão já registrada em `Plano_Ingestion.md` ("Grafana unificando PostgreSQL/métricas e
Loki/logs"); aqui fica formalizado que ambos (Loki + Promtail) entram como serviços de
infraestrutura no compose, mesma categoria de Postgres/Prometheus/Grafana — sem exigir
mudança na aplicação.

Stack de observabilidade completo: **Prometheus** (métricas) + **Loki** (logs) +
**Promtail** (coletor) + **Grafana** (visualização unificada).

### Como subir o stack de observabilidade (passo a passo)

Nenhum dos dois stacks sobe sozinho — é sempre uma ação manual (`docker compose ... up
-d`), separada do deploy do app. Comandos exatos, copiáveis:

**Dev** (na sua máquina, sob demanda — suba só quando for investigar algo, derrube
depois):
```bash
docker compose -f docker-compose.dev-observability.yml --env-file .env.dev-db up -d
```
- Precisa do `.env.dev-db` (mesmas credenciais do `docker-compose.dev-db.yml`, usadas
  pelo datasource Postgres do Grafana) — se não existir, copie de
  `.env.dev-db.example` e preencha.
- Não precisa do gateway/serviços rodando antes — o Prometheus só começa a mostrar dados
  quando o gateway nativo (`./build/.../service_gateway_http`) estiver de pé, mas o
  `up -d` em si funciona independente disso.
- **Acessar**: Grafana em `http://localhost:3301` (login padrão `admin`/`admin`,
  troca no primeiro acesso), Prometheus em `http://localhost:9091`, Loki em
  `http://localhost:3101` (raramente acessado direto — consulta-se via Grafana).
- **Derrubar quando terminar**: `docker compose -f docker-compose.dev-observability.yml down`
  (usa `down`, não só parar — sem `restart:` configurado, não volta sozinho mesmo
  parado; `down` também limpa a rede/containers do projeto `rdws_dev_obs`).

**QA** (no homelab, fica no ar — precisa do `docker-compose.qa-app.yml` já rodando
antes, pois o Prometheus escrapa o gateway pela rede do Compose):
```bash
docker compose -f docker-compose.qa-observability.yml --env-file .env.qa up -d
```
- Mesmo `.env.qa` já usado pelos outros compose de QA (`docker-compose.qa-app.yml`/
  `qa-db.yml`) — nenhuma variável nova além das que já existem.
- **Acessar** (via Tailscale, mesma máquina do homelab): Grafana em
  `http://fedora-server:3300`, Prometheus em `http://fedora-server:9091`.
- Como tem `restart: unless-stopped`, uma vez no ar fica — não precisa repetir o `up -d`
  a cada deploy do app; só rode de novo se derrubou manualmente ou trocou algo no
  compose (nesse caso `up -d` de novo recria só o que mudou).

**Dica pra quem está começando com Docker**: `docker compose ... up -d` sobe em
background (o `-d`); pra ver se subiu certo, `docker compose -f <arquivo> ps` lista os
containers e o status (`healthy`/`starting`/etc). `docker compose -f <arquivo> logs -f
<serviço>` mostra o log ao vivo de um container específico (ex. `grafana`) se algo não
aparecer no navegador.

**Portainer** (`docker-compose.infra.yml`, container `rdws_portainer`, porta 9000):
✅ dashboard web pra ver containers/volumes/redes/logs do Docker rodando no homelab —
independente de ambiente (dev/qa/prod), por isso em compose próprio (`name: rdws_infra`),
fora do ciclo de vida de qualquer projeto. Não é observabilidade da aplicação (isso é
Prometheus/Loki/Grafana acima) — é debug operacional do próprio Docker.

**RabbitMQ**: já coberto no `Plano_Ingestion.md` (fila do pipeline de ingestão) — entra
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
  `ghcr.io/rdmse/rdws_us/<service>:<sha>` (+ tag `latest` ou `qa`).
- **`.github/workflows/deploy-qa.yml`**: gatilho após build bem-sucedido na branch
  principal — roda no runner self-hosted, faz `docker compose -f docker-compose.qa-app.yml
  pull && up -d` direto no homelab (mesma máquina do runner ou via SSH local). Só o app
  — o banco (`docker-compose.qa-db.yml`) não faz parte do deploy de rotina, sobe uma vez
  e fica no ar.
- **`.github/workflows/deploy-prod.yml`**: gatilho manual (`workflow_dispatch`) ou por
  tag/release — reusa a mesma imagem já testada em QA (promoção, não rebuild), conecta
  na VPS via SSH (chave em GitHub Secrets) e roda `docker compose -f
  docker-compose.prod.yml pull && up -d`.
- Migrations do Flyway rodam como step/job dedicado antes do `up -d` dos serviços, em
  ambos os pipelines de deploy.
- **`.github/workflows/migrate-dev.yml`** (2026-07-09): dev continua 100% manual pro
  app/gateway (roda nativo fora do Docker), mas o banco `rdws_dev` é compartilhado
  (fica na homelab) — esse workflow só roda o Flyway a cada push na branch `dev`,
  mantendo o schema sincronizado automaticamente sem precisar de build/CI completo.
  Usa o Environment `development` do GitHub (secrets `DB_USER`/`DB_PASSWORD`/`DB_NAME`),
  que já existia mas ficava sem uso.

## 5. Documentação

Este documento é o registro das decisões tomadas — os três ambientes, como rodar
localmente (sem mudança), como rodar QA/prod via compose, e como funciona o pipeline
de CI/CD — servindo de referência para a implementação e para sessões futuras.

## 6. Ordem de implementação

1. ✅ **Dockerfile do gateway** (`service_gateway_http`) — prova de conceito isolada, valida
   o multi-stage build antes de replicar pros demais serviços.
   - Base `ubuntu:24.04` (não `debian:bookworm`): bookworm só tem `libpqxx-dev` 6.4.5,
     e o código usa API do libpqxx 7 (`pqxx::params`, `exec_prepared`/`exec(prepped,...)`).
   - `src/shared/database/postgresql_database.cpp` faz guard por `PQXX_VERSION_MAJOR/MINOR`
     para compilar sem warnings tanto no 7.8.1 (CI/Ubuntu) quanto em libpqxx ≥7.9 (dev local).
   - Validado: build completo + 134 testes via `ctest` dentro do container, binário do
     estágio `runtime` sobe e responde em `/health`/`/status`.
2. ✅ **docker-compose de QA** orquestrando o gateway (`docker-compose.qa-app.yml` — só o
   gateway por enquanto; demais containers entram nos próximos passos desta lista).
   - `RDWS_AUTH_MODE=jwt` via env (`RDWS_JWT_SECRET` obrigatório, lido de `.env.qa`
     — não versionado; template em `.env.qa.example`).
   - Portas 8080 (TCP, registro de backends) e 3001 (HTTP) publicadas no host.
   - `routes.json` persistido em volume nomeado (`gateway_data`), sobrevive a restart.
   - Validado: `docker compose -f docker-compose.qa-app.yml --env-file .env.qa up -d --build`
     sobe o gateway com auth JWT ativo (confirmado no log `auth=jwt`).
3. ✅ **Containers dos serviços** — todos os 8 (`auth`, `farm`, `field`, `device`,
   `device_config`, `sensor`, `sensor_reading`, `persistence`) em
   `docker-compose.qa-app.yml`, reaproveitando o mesmo Dockerfile genérico
   (`--build-arg SERVICE=<nome>`) e bloco `environment` compartilhado via YAML anchor
   (`x-db-env`/`*db-env`) para as credenciais de banco.
   - `Dockerfile` generalizado: agora aceita `--build-arg SERVICE=<target-cmake>`
     (default `service_gateway_http`), localizando o binário por nome em vez de caminho
     fixo (`find build -name "${SERVICE}"`) — a camada de compilação (a mais cara) não
     referencia `SERVICE`, então fica em cache entre builds de serviços diferentes.
   - **Bug real encontrado e corrigido**: `ServiceClient::createConnection` (TCP) usava
     `inet_pton` — só aceita IP literal, não resolve hostname. O `Plano_Deployment.md`
     já afirmava (§2) que nomes de serviço do Compose funcionariam como DNS
     (`tcp://gateway:8080`) sem mudança de código, o que era falso. Trocado por
     `getaddrinfo` (`src/service_broker/Services/ServiceClient.cpp`), que resolve tanto
     IP quanto hostname. Sem essa correção, nenhum serviço containerizado conseguiria se
     conectar ao gateway via nome DNS do Compose.
   - Validado ponta a ponta: `auth_service` (`docker-compose.qa-app.yml`) conecta no
     gateway via `tcp://gateway:8080`, registra a capability `auth.login`; `POST
     /invoke/auth.login` com o usuário seed (`admin`/`changeme`) retorna JWT; o gateway
     valida esse JWT em `/invoke` (401 sem token, passa com token — mesmo
     `RDWS_JWT_SECRET`/`JWT_SECRET` compartilhado entre os dois serviços).
   - 134/134 testes (`ctest`) continuam passando após a mudança no `ServiceClient`.
   - Validado ponta a ponta com os 8 serviços de uma vez: `GET /status` mostra
     `totalServices: 8, healthyServices: 8` (todas as capabilities registradas); `POST
     /invoke/auth.login` → JWT → `POST /invoke/farm.list` com o token retorna 200 (401
     sem token) — cobre o critério de verificação do §Verificação ("chamada autenticada
     fim-a-fim: login → CRUD num serviço").
4. ✅ **PostgreSQL containerizado** — `rdws_dev` (`docker-compose.dev-db.yml`) e `rdws_qa`
   (`docker-compose.qa-db.yml`) no homelab, imagem `postgis/postgis:16-3.4`.
   `rdws_prod` na VPS fica pra depois (junto do compose de prod).
   - **Refatoração banco/app separados**: banco e app de QA viraram dois arquivos
     (`docker-compose.qa-db.yml` / `docker-compose.qa-app.yml`) em vez de um só, cada um
     com ciclo de vida próprio (redeploy do app não mexe no banco). Os dois usam o mesmo
     `name: rdws_qa` de propósito — isso os coloca no mesmo projeto/rede Compose
     (`rdws_qa_default`) mesmo rodando via comandos `up` separados, então o gateway
     resolve `postgres` por DNS sem precisar declarar rede externa. Validado: `docker
     exec rdws_gateway_qa cat /etc/hosts` mostra a entrada `postgres` resolvida
     corretamente após subir os dois arquivos em sequência.
   - Mesmo padrão vale para prod quando chegar a hora: `docker-compose.prod-db.yml` +
     `docker-compose.prod-app.yml`.
   - Homelab foi reformatado (ambiente limpo) — sem dados a migrar do antigo Postgres
     nativo (`db/init_server.sh` fica obsoleto, mantido só de referência histórica).
   - `rdws_dev` publica a porta 5432 no host (acesso via Tailscale do notebook);
     `rdws_qa` publica 5433→5432 (atualizado em 2026-07-09 — inicialmente sem porta
     publicada, mudou pra permitir inspeção via DBeaver/psql direto via Tailscale; porta
     diferente de 5432 pra não colidir com `rdws_dev` no mesmo host). Serviços do
     próprio compose de QA continuam acessando via DNS interno (`postgres:5432`), a
     porta publicada é só pra acesso externo de ferramentas.
   - Cada compose tem `name:` explícito (`rdws_dev_db` / `rdws_qa`) — **achado
     importante**: sem isso, os dois arquivos compartilhariam o mesmo nome de projeto
     Compose (derivado do diretório) e colidiriam na mesma rede, fazendo um serviço
     `postgres` recriar o do outro arquivo.
   - Migrations via serviço `migrate` (imagem `flyway/flyway:10`, profile `migrate`):
     `docker compose -f docker-compose.<dev-db|qa>.yml --env-file <arquivo> --profile migrate run --rm migrate`.
   - Validado localmente: os dois sobem lado a lado sem conflito, migrations aplicadas
     (schema `flyway_schema_history` em v2, extensões `postgis`/`pgcrypto` ativas),
     `rdws_dev` acessível externamente, `rdws_qa` não.
5. ⏸️ **RabbitMQ containerizado** — adiado. Depende do `IngestionService` e
   `ReadingWriterService` (`Plano_Ingestion.md`), que ainda não existem no código — sem
   nada publicando/consumindo, não dá pra validar o container de ponta a ponta como os
   demais passos desta lista. Na prática entra junto do `SensorSimulatorService` (passo 8),
   que é o gerador de carga que alimenta esse pipeline; RabbitMQ deve ser containerizado
   quando o `IngestionService`/`ReadingWriterService` forem implementados, não antes.
   Pulado por ora — seguindo pro passo 6 (observabilidade), que já tem o que observar
   (gateway + 8 serviços rodando).
6. ✅ **Prometheus + Loki + Promtail + Grafana containerizados**.
   `docker-compose.qa-observability.yml` + `docker-compose.dev-observability.yml` (ver §3
   para o detalhe da separação por ambiente). `GET /metrics/prometheus` novo no gateway
   (ver §3). Portainer (infra, não observabilidade de app) já estava ✅ desde antes, em
   `docker-compose.infra.yml`.
   - Validado QA: Prometheus raspa `gateway:3001/metrics/prometheus` via DNS interno
     (target `up`, query `rdws_gateway_services_healthy` retorna valor real); Grafana
     provisiona os 2 datasources (`Prometheus`/`Loki`, UIDs fixos) e o dashboard
     (`rdws-gateway-overview`) automaticamente, sem setup manual; Promtail (Docker SD,
     filtrado por `com.docker.compose.project=rdws_qa`) entrega logs no Loki, consultável
     via `{env="qa"}` — inclusive logs dos próprios containers de infra (Grafana).
   - Validado dev: rodando o gateway nativo no host (`./build/.../service_gateway_http`),
     Prometheus alcança via `host.docker.internal:3001` (target `up`); Promtail lê
     `logs/rdws-gateway.log` (montado read-only) e entrega no Loki, consultável via
     `{job="rdws_dev"}` — inclusive os logs JSON estruturados que o `logger.cpp` já
     escrevia, sem precisar mudar nada na aplicação.
7. 🟡 **CI/CD** (GitHub Actions: build → GHCR → deploy-qa → deploy-prod) — por último, pra
   automatizar um fluxo que já foi validado manualmente em cada etapa anterior.
   - ✅ `ci.yml` (build + `ctest` no runner self-hosted) validado com PR real (ver
     `Plano_Gateway_HTTP.md` Fase 10b para o detalhe do bug de submodules corrigido).
   - ✅ `deploy-qa.yml` já existia e funciona (build local no runner self-hosted, sem
     depender do GHCR — homelab builda e sobe direto).
   - ✅ Job `push-ghcr` adicionado ao `ci.yml` (matrix por serviço, publica
     `ghcr.io/rdmse/rdws_us/<service>:<sha|qa|latest>` só em push na `main`, usando
     `GITHUB_TOKEN`, sem secret novo).
   - ✅ `docker-compose.prod-db.yml`/`docker-compose.prod-app.yml` criados (mesmo padrão
     de QA, mas puxando imagem do GHCR via `IMAGE_TAG` em vez de buildar; limites de
     memória por serviço somando ao teto de 1GB da VPS) e `deploy-prod.yml` criado
     (gatilho `workflow_dispatch` com input `image_tag`, `environment: production`,
     roda via `DOCKER_HOST=ssh://...` contra a VPS, sem rebuild — promoção de imagem já
     testada em QA). `.env.prod.example` documenta as variáveis esperadas.
   - ⬜ **Pendente, fora do alcance de automação**: provisionar a VPS (Docker instalado,
     chave SSH autorizada), criar o GitHub Environment `production` com os secrets/vars
     reais (`PROD_SSH_KEY`, `PROD_SSH_USER`, `PROD_HOST`, `DB_PASSWORD`,
     `RDWS_JWT_SECRET` de prod) e disparar o primeiro `workflow_dispatch` de verdade.
8. **`SensorSimulatorService`** — só depois de tudo dockerizado e rodando em QA e prod
   (etapas 1-7). Fica de fora do compose principal (aplicação separada, ver §1); com o
   pipeline de ingestão já estável, plano próprio detalha seu desenho.
9. **Escalabilidade horizontal do gateway** — backlog, sem data. O `HttpGateway` hoje
   assume instância única (conexões de socket com backends, config/rotas em arquivo local
   por instância); ver `Plano_Gateway_HTTP.md` (Fase 14) para o levantamento completo e o
   plano de migração de `routes.json`/`GatewayConfig` para o banco. Só entra depois de
   todas as etapas acima estarem estáveis em produção.
10. **Habilitar Log volume no Loki** — backlog, sem data. `infra/loki/loki-config.yml`
    não tem `limits_config.volume_enabled: true`; sem isso, o recurso de "Log volume"
    do Explore do Grafana (histograma de volume de logs) mostra aviso de não configurado.
    Não afeta o dashboard principal (`gateway-overview.json`), só o Explore.

## Verificação

1. `docker compose -f docker-compose.qa-db.yml up -d && docker compose -f
   docker-compose.qa-app.yml up -d` no homelab e confirmar via `GET /health` do broker
   que todos os serviços se registraram.
2. Confirmar que uma chamada autenticada fim-a-fim (login no `auth_service` → CRUD num
   serviço, ex. `farm_service`) funciona através do broker containerizado.
3. Confirmar que o Flyway aplicou as migrations (`flyway_schema_history` populada) no
   banco correto de cada ambiente (`rdws_dev`/`rdws_qa`/`rdws_prod`) e o
   `seed_fake_data.sql` roda sem erro.
4. Verificar `curl localhost:9090/targets` (Prometheus) mostrando todos os alvos "up", e
   o dashboard Grafana carregando dados reais (métricas + logs via Loki).
5. Rodar o workflow `deploy-qa` manualmente uma vez e conferir logs do runner
   self-hosted; só depois habilitar `deploy-prod` contra a VPS.
