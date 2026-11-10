Data: 2026-06-17

# Plano API REST - Sensor Analysis System

Cada recurso é um microserviço independente que se conecta ao gateway pelo socket existente e se registra com suas capabilities. O gateway roteia as requisições HTTP para o microserviço correto via EventRouter, usando a combinação `método HTTP + path` para resolver a capability.

---

## Endpoints e Capabilities

### AuthService

| Método | Path          | Capability   | Descrição                              |
|--------|---------------|--------------|----------------------------------------|
| POST   | /auth/login   | auth.login   | Recebe credenciais, retorna JWT Bearer |

> Endpoint público — bypass de autenticação no gateway. Todos os demais endpoints requerem Bearer token.

---

### FarmService

| Método   | Path          | Capability      | Descrição                 |
|----------|---------------|-----------------|---------------------------|
| GET      | /farms        | farm.list       | Lista todas as fazendas   |
| GET      | /farms/{id}   | farm.get        | Busca fazenda por id      |
| POST     | /farms        | farm.create     | Cria nova fazenda         |
| PUT      | /farms/{id}   | farm.update     | Atualiza fazenda          |
| DELETE   | /farms/{id}   | farm.delete     | Remove fazenda            |

### FieldService

| Método   | Path           | Capability     | Descrição                       |
|----------|----------------|----------------|---------------------------------|
| GET      | /fields        | field.list     | Lista campos (filtro: farm_id)  |
| GET      | /fields/{id}   | field.get      | Busca campo por id              |
| POST     | /fields        | field.create   | Cria novo campo                 |
| PUT      | /fields/{id}   | field.update   | Atualiza campo                  |
| DELETE   | /fields/{id}   | field.delete   | Remove campo                    |

### DeviceService

| Método   | Path             | Capability       | Descrição                         |
|----------|------------------|------------------|-----------------------------------|
| GET      | /devices         | device.list      | Lista devices (filtro: field_id)  |
| GET      | /devices/{id}    | device.get       | Busca device por id               |
| POST     | /devices         | device.create    | Registra novo device              |
| PUT      | /devices/{id}    | device.update    | Atualiza device                   |
| DELETE   | /devices/{id}    | device.delete    | Remove device                     |

#### ✅ Rotação e revogação de credencial via REST (2026-10-05)

`device_credential.rotate` e `device_credential.revoke` existem no `DeviceService`
(`Plano_DeviceCredentials.md` §6) e eram **internos de propósito** (fora do `routes.json`)
até 2026-10-05. Sem rota HTTP, uma chave vazada não podia ser trocada sem apagar e
recriar o device. Caso real: a PSK do device 12 (QA, `rdws_weather_node` de bancada)
apareceu num log de debug em 2026-10-02 e ficou sem rotação por falta dessa rota.

| Método | Path                                | Capability                | Descrição                                             |
|--------|-------------------------------------|---------------------------|-------------------------------------------------------|
| POST   | /devices/{id}/credential/rotate     | device_credential.rotate  | Gera nova PSK (retornada uma única vez), revoga a atual |
| POST   | /devices/{id}/credential/revoke     | device_credential.revoke  | Revoga a PSK ativa                                    |

- Restrito a `admin`, porque é operação sensível. A resposta do `rotate` traz a chave
  em claro, como no `device.create`. A checagem fica no handler (`requireAdmin()` em
  `AppDeviceService.cpp`): com identidade injetada pelo gateway, exige a claim
  `role = admin` (senão `403`); sem identidade (modo `none` em dev, ou invoke interno),
  passa, como as demais rotas.
- O id vem do path (`{id}`); invokes internos ainda podem mandar `device_id` no corpo.
- Requests no Bruno: `Devices/Rotate Device Credential` e `Revoke Device Credential`.
- Quando o `rotate` passar a valer pra hardware em campo, entra o grace period que o
  `Plano_DeviceCredentials.md` §6 adiou.
- ✅ PSK do device 12 rotacionada e placa reprovisionada com `rdws psk set` (2026-10-05).

### DeviceConfigService

| Método   | Path                         | Capability              | Descrição                       |
|----------|------------------------------|-------------------------|---------------------------------|
| GET      | /devices/{id}/config         | device_config.get       | Busca configuração do device (com `version`) |
| PATCH    | /devices/{id}/config         | device_config.update    | Atualiza por *merge patch* (RFC 7396)        |

- A config é 1:1 com o device e nasce vazia (`{}`) junto com ele, por trigger (V3); por isso
  não há `create` nem `delete` (rotas removidas no RDWS-16). `version` é controlada pelo banco
  (V12) e sobe a cada mudança.
- No `PATCH`, chave omitida fica como está, `null` remove a chave e uma lista é substituída
  inteira. Para `weather_station` real, a config mesclada é validada contra um JSON Schema
  estrito, os `onboard[].sensor_id` precisam ser sensores do device do tipo do canal, e as
  regras de `triggers[]` são conferidas contra o resto da config (RDWS-172). Erro → `400`.
- Chaves aceitas pelo firmware: README do `rdws_weather_node`, "Configuração pelo backend".

### SensorService

| Método   | Path             | Capability       | Descrição                           |
|----------|------------------|------------------|-------------------------------------|
| GET      | /sensors         | sensor.list      | Lista sensores (filtro: device_id)  |
| GET      | /sensors/{id}    | sensor.get       | Busca sensor por id                 |
| POST     | /sensors         | sensor.create    | Registra novo sensor                |
| PUT      | /sensors/{id}    | sensor.update    | Atualiza sensor                     |
| DELETE   | /sensors/{id}    | sensor.delete    | Remove sensor                       |

### SensorReadingService *(somente leitura — append-only)*

| Método   | Path                          | Capability              | Descrição                                      |
|----------|-------------------------------|-------------------------|------------------------------------------------|
| GET      | /sensors/{id}/readings        | sensor_reading.list     | Lista leituras por sensor e janela de tempo    |
| GET      | /sensors/{id}/readings/{rid}  | sensor_reading.get      | Busca leitura específica por id                |

---

## Roteamento no EventRouter

O EventRouter resolve a capability pela combinação `method + path`. Exemplo de regras:

```json
{ "method": "GET",    "path": "/farms",      "capability": "farm.list"   },
{ "method": "GET",    "path": "/farms/{id}", "capability": "farm.get"    },
{ "method": "POST",   "path": "/farms",      "capability": "farm.create" },
{ "method": "PUT",    "path": "/farms/{id}", "capability": "farm.update" },
{ "method": "DELETE", "path": "/farms/{id}", "capability": "farm.delete" }
```

Os `pathParameters` (ex: `{id}`) são extraídos pelo gateway e injetados no `LambdaEvent`, disponíveis para o microserviço sem parsing adicional.

---

## Contrato de Payload

Todos os requests seguem o contrato `LambdaEvent` já estabelecido no gateway.

### Exemplos

**GET /farms/42**
```json
{
  "pathParameters": { "id": "42" }
}
```

**POST /farms**
```json
{
  "body": {
    "name": "Fazenda São João",
    "location": { "lat": -15.7801, "lng": -47.9292 }
  }
}
```

**GET /sensors/7/readings**
```json
{
  "pathParameters": { "id": "7" },
  "queryStringParameters": {
    "from": "2026-06-01T00:00:00Z",
    "to": "2026-06-17T23:59:59Z"
  }
}
```

---

## Observações

- `SensorReadingService` não expõe POST/PUT/DELETE — é somente leitura via REST. A escrita de
  leituras de sensor é um pipeline totalmente separado do gateway HTTP: device → CoAP →
  `IngestionService` → RabbitMQ → `ReadingWriterService` → banco (ver `Plano_Ingestion.md`).
  Não confundir com o `PersistenceService` da Fase 10a do `Plano_Gateway_HTTP.md`, que grava
  apenas telemetria operacional do próprio gateway (`request_history`/`capability_metrics`),
  sem relação com leituras de sensor.
- Filtros hierárquicos (ex: campos de uma fazenda) são passados via `queryStringParameters` (`farm_id`, `field_id`, `device_id`).
- Todos os microserviços se autenticam pelo mesmo mecanismo do gateway (API key ou JWT Bearer).
- Implementação de cada microserviço segue o mesmo padrão dos demais serviços existentes no projeto.
