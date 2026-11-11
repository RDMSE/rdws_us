# Schemas de validação do gateway

## Schemas de capability (em uso)

O `HttpGateway` valida o corpo de cada capability de escrita **antes** de despachar para o
serviço: um corpo inválido recebe `400` direto do gateway, com a lista de erros, e nem chega
ao serviço de domínio (Fase 13b do `docs/Plano_Gateway_HTTP.md`). O mapa capability →
schema fica no `CapabilitySchemaRegistry` (`capability_schema_registry.h/.cpp`); capability
sem schema registrado (leituras, deletes) não é validada aqui.

| Arquivo | Capabilities |
|---|---|
| `farm_schemas.h` | `farm.create`, `farm.update` |
| `field_schemas.h` | `field.create`, `field.update` |
| `device_schemas.h` | `device.create`, `device.update` |
| `sensor_schemas.h` | `sensor.create`, `sensor.update` |
| `device_config_schemas.h` | `device_config.update` (só a forma do envelope `{"config": {...}}`) |

O conteúdo da config de uma `weather_station` (chaves, faixas, `onboard[]`, `triggers[]`) é
validado depois, no `DeviceConfigService`, sobre a config **já mesclada**, com o schema estrito
de `src/shared/service/DeviceConfigSchemas.h`: depende do tipo do device e do resto da config,
o que o gateway não conhece.

Para acrescentar uma capability: um `const std::string <NOME>_SCHEMA` (JSON Schema draft-07)
num `*_schemas.h`, o registro no construtor do `CapabilitySchemaRegistry` e um caso no
`src/service_broker/tests/test_http_e2e.cpp`.
