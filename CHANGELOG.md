# Changelog — rdws_us

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/); versões em
[SemVer](https://semver.org/lang/pt-BR/), pela tag git (README, "Versões"). Cada entrada traz a
versão da API e a do schema do banco.

## Compatibilidade

| `rdws_us` | API (`apiVersion`) | Schema (Flyway) | `rdws_web` | Estação (`rdws_weather_node`) |
|---|---|---|---|---|
| Não lançado | 1 | V16 | — (não iniciado) | SenML JSON, CoAP Content-Format 110 (`Plano_Telemetria.md`) |

## [Não lançado]

Primeira versão marcada. Registra o estado da solução antes da separação open-core (o que
fica no repositório privado está em `docs/Backlog.md` e na memória do plano).

### Plataforma
- Gateway HTTP com service broker (TCP/UNIX), roteamento por capability, validação de input por
  schema, autenticação JWT e role `admin` nos endpoints administrativos (`/routes`, `/config`,
  `POST /events`).
- Microserviços: auth, farm, field, device (inclui as credenciais PSK: rotação e revogação),
  device_config, sensor, sensor_reading e persistence; simulador de estações para o QA.
- Versão da solução e da API em `GET /status`, nos serviços e nas imagens.

### Ingestão
- `IngestionService` (CoAP/DTLS-PSK, identidade pelo handshake), só SenML JSON: o JSON legado
  responde `4.15`.
- Publisher confirms em lote no RabbitMQ; `5.03` quando o pack não foi aceito, e a estação
  reenvia.
- Faixas plausíveis por unidade: leitura fora da faixa gravada com o flag `out_of_range`
  (`0x4000`).
- `ReadingWriterService` resiliente a banco e RabbitMQ fora (SQLSTATE, reconexão,
  dead-letter `.dead`, prefetch 100).
- `rec_cfg` → `device_config_spans` (versão da config de cada leitura); `lat`/`lon` →
  `devices.location`.

### Configuração da estação
- Config versionada pelo banco, entregue na resposta CoAP (piggyback), com histórico
  (`device_config_history`).
- `GET /devices/{id}/config/status`: se a versão atual roda na estação (`applied`, `pending`,
  `rejected` ou `unknown`), a partir do último `cfg_version`/`cfg_rej` que ela declarou
  (RDWS-180).
- `GET /devices/{id}/diagnostics`: o snapshot de diagnóstico mais recente do device. O
  `device_service` é o dono da leitura de `device_telemetry`; o `device_config_service` a
  consulta pelo gateway (`DeviceDiagnosticsClient`), não pela tabela (RDWS-180).
- `triggers[]` (edge triggers) validados no backend e convertidos para o ponto fixo do firmware
  na entrega.

### Observabilidade
- Grafana: Farm Overview (leituras, diagnósticos com `cfg_rej`/`up_rej`/`trg`, falhas de
  firmware), Gateway Overview, Fleet Health; alertas de frota "Silent station" e
  "Restart loop".

### Licença
- AGPL-3.0-only, com `src/shared/` e o client do broker em Apache-2.0; cabeçalhos SPDX
  checados no CI.
