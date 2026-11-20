# Changelog — rdws_us

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões em
[SemVer](https://semver.org/lang/pt-BR/), pela tag git (README, "Versões"). Cada entrada traz a
versão da API e a do schema do banco. Cada linha termina com a issue de origem (`[RDWS-<n>]`,
links no fim do arquivo); as anteriores ao fluxo de issues não têm referência.

## Compatibilidade

| `rdws_us` | API (`apiVersion`) | Schema (Flyway) | `rdws_web` | Estação (`rdws_weather_node`) |
|---|---|---|---|---|
| v0.4.0 | 2 | V22 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |
| v0.3.0 | 2 | V22 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |
| v0.2.0 | 2 | V22 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |
| v0.1.0 | 1 | V16 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |

## [Não lançado]

### Documentação
- `Plano_Indices_Derivados.md` e `Plano_Alerting.md` ficam só com o que é da plataforma: a
  config de cada leitura (D1.1), a altitude do device e o fuso da fazenda (D7), os triggers do
  device (Camada 1), a plausibilidade e a cópia das leituras pela fanout. O cálculo dos índices
  e a avaliação de alarmes saem deste repositório, com as seções correspondentes do
  `Backlog.md` (separação open-core, passo 5) ([RDWS-221]).

### Plataforma
- O `Dockerfile` instala os pacotes do Ubuntu de um mirror do país (`ARG UBUNTU_MIRROR`, padrão
  `http://at.archive.ubuntu.com/ubuntu`, também para o `-security`) nos dois estágios: em
  2026-11-19 o `archive.ubuntu.com` e o `security.ubuntu.com` ficaram horas inacessíveis daqui,
  e um build sem as camadas do apt em cache teria falhado. Muda com `--build-arg` ([RDWS-224]).

### Observabilidade
- O mapa do Farm Overview volta a ter fundo: a camada "default" dos mapas do Grafana passa a ser
  o OpenStreetMap (`GF_GEOMAP_DEFAULT_BASELAYER_CONFIG`, nos compose de dev e de QA), que não
  pede chave; o padrão do Grafana (CARTO) passou a pedir uma. Vale para todo mapa novo, e o
  painel não muda. O Grafana fica fixo na versão 13.1.0 (era `latest`): atualizar passa a ser
  uma mudança deliberada ([RDWS-223]).

### Ingestão
- Registros com instante impossível, antes de 2020-01-01 ou mais de 1 h à frente do relógio do
  servidor, são descartados: leituras e diagnósticos, contados em `invalid_time` na linha de log
  do pack, que continua sendo confirmado. Pega o RTC em 2000-01-01 depois de um reset e o
  contador desde o boot lido como tempo relativo (`Plano_Telemetria.md`) ([RDWS-225]).
## [v0.4.0] — 2026-11-17

As leituras passam pela exchange fanout `rdws.sensor_readings` (passo 3 da separação open-core),
e o `rdws_us` passa a ser incluível como dependência sem chave SSH: o que o repositório privado
`rdws_enviro` (passo 4) precisa. Sem mudança de API nem de schema.

### Plataforma
- Os submódulos (inih, valijson, dotenv-cpp) passam a ser clonados por HTTPS: um projeto que
  inclui o `rdws_us` (por `FetchContent` ou submódulo) não precisa mais de chave SSH no GitHub
  ([RDWS-219]).

### Ingestão
- As leituras passam por uma exchange fanout (`rdws.sensor_readings`): cada consumidor recebe a
  própria cópia, numa fila sua. O writer continua na fila `sensor_readings`; a ingestion
  declara essa fila e a ligação, para nenhuma leitura se perder se ela subir antes do writer.
  Prepara o `AlertingService` do repositório privado ([RDWS-216]).

## [v0.3.0] — 2026-11-17

Ponto estável antes do passo 3 da separação open-core (a exchange fanout, que mexe no caminho de
escrita das leituras). Sem mudança de API nem de schema. O client do broker vira biblioteca
própria e ganha a detecção de conexão meio-aberta; o CI e o deploy de QA passam a usar uma imagem
só, e do merge ao QA atualizado caiu de ~21 para ~4 min.

### Plataforma
- O client do broker detecta uma conexão meio-aberta (gateway travado, ou a rede caiu sem
  fechar a conexão): sem nenhuma mensagem do broker por 75 s, desconecta e o laço de reconexão
  assume. Antes, o `recv()` ficava bloqueado para sempre e o serviço parecia no ar sem receber
  nada ([RDWS-212]).
- O client do broker (`ServiceClient`, `ServiceIdentity`) virou a biblioteca `rdws_broker_client`
  em `src/shared/broker/`, Apache-2.0 como o resto de `src/shared`. Os serviços a linkam no lugar
  do gateway inteiro (só o `auth_service` ainda usa o gateway, para assinar o JWT). Outro projeto
  pode incluir o `rdws_us` e compilar só as bibliotecas compartilhadas (`RDWS_SHARED_ONLY`),
  testado no CI por `tests/consumer/` ([RDWS-201]).
- `.dockerignore` tira do contexto do build o que não é código (docs, Bruno, infra, CI, os
  `.env` gerados pelos deploys): o deploy de QA reaproveita a compilação do CI e um commit só de
  docs não recompila nada ([RDWS-205]).
- **Uma imagem só para todos os serviços** (`ghcr.io/rdmse/rdws_us/services`): o estágio de
  runtime leva os binários que o builder compilou e testou, e cada container escolhe o dele
  por `RDWS_SERVICE`. O CI a constrói e publica uma vez; o deploy de QA a puxa pelo SHA do
  commit em vez de construir. Saem os 12 pacotes por serviço no GHCR e o job `push-ghcr`.
  Do merge ao QA atualizado: de ~21 min para ~7 min ([RDWS-205]).
- A versão da solução vai para a imagem como variável de ambiente (`RDWS_VERSION`, lida por
  `rdws::version()`), e não mais compilada: o CI do merge reaproveita a compilação do PR em vez
  de compilar tudo de novo, porque a versão muda a cada commit ([RDWS-210]).

### Ingestão
- No QA, a ingestion se registra no gateway com o ambiente `qa` (antes, `test`: faltava o
  `RDWS_ENVIRONMENT` no `docker-compose.qa-app.yml`) ([RDWS-203]).
- Quando a primeira carga dos caches (credenciais, donos dos sensores) falha, por exemplo porque
  a ingestion subiu antes do `device_service` num deploy, ela tenta de novo a cada 5 s, e não
  só no ciclo de 60 s. Antes, nesse minuto todo handshake DTLS era recusado ([RDWS-199]).

## [v0.2.0] — 2026-11-13

A API passa a endereçar farms, fields, devices e sensors pelo `public_id` (UUID), Fase 13a do
`Plano_Gateway_HTTP.md`: quebra o contrato, por isso `apiVersion` 2. Os ids internos seguem no
banco, no Grafana e no protocolo da estação.

### Configuração da estação
- `GET /devices/{id}/config/status`: se a versão atual roda na estação (`applied`, `pending`,
  `rejected` ou `unknown`), a partir do último `cfg_version`/`cfg_rej` que ela declarou
  ([RDWS-180]).
- `GET /devices/{id}/diagnostics`: o snapshot de diagnóstico mais recente do device. O
  `device_service` é o dono da leitura de `device_telemetry`; o `device_config_service` a
  consulta pelo gateway (`DeviceDiagnosticsClient`), não pela tabela ([RDWS-180]).

- `farms.timezone` (fuso IANA, padrão `America/Sao_Paulo`, validado contra o
  `pg_timezone_names`) e `devices.elevation_m`/`elevation_source` (altitude informada pela API
  como `manual`), para os índices derivados ([RDWS-182]).

### Plataforma
- `JsonObj` grava `null` (`setNull`, `setOrNull`) ([RDWS-180]).
- `db/seeds/seed_fake_data.sql` preenche as cópias do `public_id` do pai, obrigatórias desde a
  V19 ([RDWS-28]).

### Segurança
- Farms, fields, devices e sensors endereçados pelo `public_id` (UUID) em vez do id sequencial,
  que permitia enumerar todos: o `id` nas respostas e nos paths (`/farms/{id}`, `/fields/{id}`,
  `/devices/{id}`, inclusive `diagnostics` e `credential/rotate|revoke`, `/sensors/{id}`) e as
  referências ao pai (`farm_id` dos fields, `field_id` dos devices, `device_id` dos sensors:
  corpo, filtro e resposta) agora são UUIDs, e um
  id numérico responde `400`. `PUT` e `DELETE` de um recurso inexistente respondem `404` (antes,
  sucesso). O filho guarda o `public_id` do pai (`fields.farm_public_id`,
  `devices.field_public_id`, `device_configurations.device_public_id`,
  `sensors.device_public_id`, FK composta com o id interno), para nenhum serviço ler a tabela de
  outro. `POST /sensors` passa a responder `404` para um device inexistente (antes, a FK falhava
  com `500`). `/devices/{id}/config` e
  `/devices/{id}/config/status` também recebem o UUID, e o `deviceId` da config passa a ser ele
  (o `id` da linha de config, interno, saiu da resposta). A estação não muda: o protocolo CoAP
  continua com os ids internos (`bn` do SenML, `sensor_id` da config); a ingestion pede a config
  pelo UUID que o cache de credenciais agora traz. Exceção: o `onboard[].sensor_id` da config
  continua o id interno do sensor, porque é o do protocolo da estação (backlog).
  `/sensors/{id}/readings` recebe o UUID do sensor (desconhecido: `404`, não lista vazia), o
  `sensor_id` das leituras vem em UUID, e `/sensors/{id}/readings/{rid}` só acha uma leitura
  daquele sensor: antes, qualquer `rid` aparecia sob qualquer sensor. O `id` da leitura continua
  o sequencial, alcançável só sob o próprio sensor. `apiVersion` passa a `2` ([RDWS-28]).
- `POST /invoke/{capability}` só encaminha capabilities expostas por uma regra de rota; antes,
  qualquer token válido chamava capabilities só de serviço, como as que devolvem as chaves PSK
  ativas (`device_credential.get_active`/`list_active`). Essas duas também recusam (`403`) uma
  requisição com identidade de cliente HTTP ([RDWS-188]).

### Corrigido
- Gateway: quando o serviço respondia antes de o handler HTTP começar a esperar, o pedido era
  descartado como se fosse interno e o cliente recebia `404 Request not found` de uma
  requisição bem-sucedida. Agora só os pedidos internos marcados como fire-and-forget (as
  pontes de persistência) são descartados na resposta ([RDWS-184]).

## [v0.1.0] — 2026-11-10

Primeira versão marcada. Registra o estado da solução antes da separação open-core (o que
fica no repositório privado está em `docs/Backlog.md` e na memória do plano).

### Plataforma
- Gateway HTTP com service broker (TCP/UNIX), roteamento por capability, validação de input por
  schema e autenticação JWT.
- Role `admin` nos endpoints administrativos do gateway (`/routes`, `/config`, `POST /events`)
  ([RDWS-155]).
- Microserviços: auth, farm, field, device (inclui as credenciais PSK: rotação e revogação),
  device_config, sensor, sensor_reading e persistence; simulador de estações para o QA.
- Versão da solução e da API em `GET /status`, nos serviços e nas imagens ([RDWS-178]).

### Ingestão
- `IngestionService` (CoAP/DTLS-PSK, identidade pelo handshake), só SenML JSON: o JSON legado
  responde `4.15` ([RDWS-165]).
- Publisher confirms em lote no RabbitMQ; `5.03` quando o pack não foi aceito, e a estação
  reenvia ([RDWS-157]); o simulador envia em packs de até 200 leituras ([RDWS-161]).
- Faixas plausíveis por unidade: leitura fora da faixa gravada com o flag `out_of_range`
  (`0x4000`) ([RDWS-174]).
- `ReadingWriterService` resiliente a banco e RabbitMQ fora (SQLSTATE, reconexão,
  dead-letter `.dead`, prefetch 100) ([RDWS-157]).
- `rec_cfg` → `device_config_spans` (versão da config de cada leitura) ([RDWS-167],
  [RDWS-169]); `lat`/`lon` → `devices.location` ([RDWS-165]).

### Configuração da estação
- Config versionada pelo banco, entregue na resposta CoAP (piggyback), com histórico
  (`device_config_history`, [RDWS-153]).
- `triggers[]` (edge triggers) validados no backend e convertidos para o ponto fixo do firmware
  na entrega ([RDWS-172]).

### Observabilidade
- Grafana: Farm Overview (leituras, diagnósticos, falhas de firmware), Gateway Overview, Fleet
  Health; alerta de frota "Silent station".
- Colunas `cfg_rej`/`up_rej`/`trg`, tabela "Firmware crashes" e alerta "Restart loop"
  ([RDWS-176]); leituras fora da faixa marcadas nas tabelas e fora dos gráficos ([RDWS-174]).

### Licença
- AGPL-3.0-only, com `src/shared/` e o client do broker em Apache-2.0; cabeçalhos SPDX
  checados no CI ([RDWS-163]).

[RDWS-28]: https://github.com/RDMSE/rdws_us/issues/28
[RDWS-153]: https://github.com/RDMSE/rdws_us/issues/153
[RDWS-155]: https://github.com/RDMSE/rdws_us/issues/155
[RDWS-157]: https://github.com/RDMSE/rdws_us/issues/157
[RDWS-161]: https://github.com/RDMSE/rdws_us/issues/161
[RDWS-163]: https://github.com/RDMSE/rdws_us/issues/163
[RDWS-165]: https://github.com/RDMSE/rdws_us/issues/165
[RDWS-167]: https://github.com/RDMSE/rdws_us/issues/167
[RDWS-169]: https://github.com/RDMSE/rdws_us/issues/169
[RDWS-172]: https://github.com/RDMSE/rdws_us/issues/172
[RDWS-174]: https://github.com/RDMSE/rdws_us/issues/174
[RDWS-176]: https://github.com/RDMSE/rdws_us/issues/176
[RDWS-178]: https://github.com/RDMSE/rdws_us/issues/178
[RDWS-180]: https://github.com/RDMSE/rdws_us/issues/180
[RDWS-182]: https://github.com/RDMSE/rdws_us/issues/182
[RDWS-184]: https://github.com/RDMSE/rdws_us/issues/184
[RDWS-188]: https://github.com/RDMSE/rdws_us/issues/188
[RDWS-199]: https://github.com/RDMSE/rdws_us/issues/199
[RDWS-201]: https://github.com/RDMSE/rdws_us/issues/201
[RDWS-203]: https://github.com/RDMSE/rdws_us/issues/203
[RDWS-205]: https://github.com/RDMSE/rdws_us/issues/205
[RDWS-210]: https://github.com/RDMSE/rdws_us/issues/210
[RDWS-212]: https://github.com/RDMSE/rdws_us/issues/212
[RDWS-216]: https://github.com/RDMSE/rdws_us/issues/216
[RDWS-219]: https://github.com/RDMSE/rdws_us/issues/219
[RDWS-221]: https://github.com/RDMSE/rdws_us/issues/221
[RDWS-223]: https://github.com/RDMSE/rdws_us/issues/223
[RDWS-224]: https://github.com/RDMSE/rdws_us/issues/224
[RDWS-225]: https://github.com/RDMSE/rdws_us/issues/225
