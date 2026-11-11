# Changelog — rdws_us

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões em
[SemVer](https://semver.org/lang/pt-BR/), pela tag git (README, "Versões"). Cada entrada traz a
versão da API e a do schema do banco. Cada linha termina com a issue de origem (`[RDWS-<n>]`,
links no fim do arquivo); as anteriores ao fluxo de issues não têm referência.

## Compatibilidade

| `rdws_us` | API (`apiVersion`) | Schema (Flyway) | `rdws_web` | Estação (`rdws_weather_node`) |
|---|---|---|---|---|
| Não lançado | 1 | V18 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |
| v0.1.0 | 1 | V16 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |

## [Não lançado]

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

### Segurança
- Farms endereçadas pelo `public_id` (UUID) em vez do id sequencial, que permitia enumerar
  todas: o `id` de `/farms` e de `/farms/{id}` agora é o UUID, e um id numérico no path
  responde `400`. `PUT` e `DELETE` de uma farm inexistente respondem `404` (antes, sucesso).
  Primeiro passo da Fase 13a; fields, devices e sensors seguem ([RDWS-28]).
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
