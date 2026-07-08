Data: 2026-07-01

# Plano de Ingestão de Dados de Sensores

O `PLANO_API_REST.md` define o `SensorReadingService` como somente leitura (`sensor_reading.list`, `sensor_reading.get`), mas não descreve como as leituras chegam ao sistema. Este documento cobre o caminho de escrita: como os devices enviam dados até eles ficarem persistidos no banco.

---

## Visão geral do fluxo

```
[Device] --CoAP (DTLS)--> [IngestionService] --parse/valida--> [Fila persistente] --> [ReadingWriterService] --> [Banco]
```

1. **IngestionService** abre uma porta CoAP e fica disponível para os devices se conectarem.
2. Cada device, em horários agendados, envia suas leituras para essa porta.
3. O IngestionService faz o parse do payload, valida contra a config do device (`device_config`) e publica a mensagem em uma fila durável.
4. Um **ReadingWriterService**, na outra ponta da fila, consome as mensagens e grava as leituras no banco (`sensor_reading`).

Esse desenho desacopla a taxa de chegada dos devices (que pode ser irregular, em rajadas, ou sofrer reconexões) da capacidade de escrita do banco, e dá retry/backpressure sem esforço extra.

---

## IngestionService

- Protocolo: **CoAP**, escolhido por causa do hardware low-power. CoAP roda sobre UDP, sem conexão persistente — o device acorda, envia o datagrama e volta a dormir, economizando rádio/bateria. MQTT (ex. Mosquitto) foi considerado e descartado para a entrada dos dados: exige manter uma conexão TCP com keep-alive aberta, custando mais energia do que o cenário permite.
- Com **DTLS** obrigatório — sem isso qualquer dispositivo na rede pode injetar leituras falsas.
- Autenticação por device: PSK ou certificado por device (chave associada ao registro em `DeviceService`).
- Responsabilidades:
  - Receber o payload CoAP do device.
  - Parsear para o formato interno de leitura.
  - Validar contra `device_config` (schema esperado, ranges plausíveis) — leituras fora do schema são descartadas/logadas, não publicadas na fila.
  - Publicar na fila persistente.
- Não acessa o banco diretamente — é stateless em relação à persistência.

## Fila persistente

- Candidatos avaliados:
  - **RabbitMQ** — fila durável, simples de operar, atende bem o volume esperado (poucos sensores por device, leituras agendadas, não streaming contínuo de alta frequência). Candidato natural.
  - **ActiveMQ (Artemis)** — tecnicamente viável, suporta persistência e múltiplos protocolos (MQTT/AMQP/STOMP), mas é mais pesado de operar; faz mais sentido se o resto da stack já usar JMS/Java, o que não é o caso aqui.
  - **ZeroMQ** — descartado: é uma biblioteca de mensageria, não um broker. Não tem persistência em disco, fila durável nem ack/redelivery nativos — exigiria reimplementar tudo isso manualmente, o que vai contra o requisito de persistência da fila.
  - **Mosquitto (MQTT)** — descartado como fila/broker para este ponto: tem persistência limitada (não foi desenhado como fila enterprise durável) e, mesmo se usado, ainda exigiria repassar as mensagens para uma fila persistente de verdade antes do ReadingWriterService.
- Dado o volume esperado, RabbitMQ com fila durável atende sem complexidade extra de operação.
- Mensagem deve carregar: `device_id`, `sensor_id`, `timestamp`, `valor`, `unidade` (ou o que o schema de `sensor_reading` exigir).

## ReadingWriterService

- Consome da fila e grava em `sensor_reading`.
- **Idempotência obrigatória**: se o consumer falhar e a mensagem for reprocessada, a escrita não pode duplicar a leitura. Usar chave única `(device_id, sensor_id, timestamp)` no banco.
- Não expõe endpoints HTTP — é um worker puro, sem capability registrada no gateway.

---

## Pontos em aberto

- Definir qual broker de fila será adotado (RabbitMQ é o candidato natural dado o resto da stack).
- Definir o mecanismo de provisionamento de credenciais DTLS por device (na criação via `device.create`?).
- Definir política de retry/DLQ no ReadingWriterService para mensagens que falham repetidamente.

## Observabilidade da fila

O `PLANO_GATEWAY_HTTP.md` (Fase 11) já define Grafana como ferramenta de observabilidade do sistema, unificando PostgreSQL (métricas/requests) e Loki (logs). RabbitMQ expõe métricas nativamente via plugin de management (Prometheus exporter desde a v3.8) — basta adicionar Prometheus como datasource no Grafana para visualizar profundidade de fila, taxa de consumo, conexões e crescimento de DLQ lado a lado com as métricas do gateway, sem introduzir ferramenta nova na stack.

## Ordem de implementação e dependências com o plano gateway

**Nota (2026-07-08, `Plano_Deployment.md` §6 passo 5):** RabbitMQ containerizado foi
adiado nesse plano de deployment — sem `IngestionService`/`ReadingWriterService`
implementados, não há nada publicando/consumindo a fila pra validar o container de
ponta a ponta. RabbitMQ só deve ser containerizado junto com a implementação destes
dois serviços (ver passo 1 abaixo), não isoladamente antes deles.

1. **Pipeline de ingestão primeiro, sem observabilidade**: simulador → IngestionService → RabbitMQ → ReadingWriterService → banco, validado ponta a ponta. Observabilidade é aditiva e não deve bloquear a funcionalidade.
2. **Fase 11 do gateway** (Grafana + Loki + Promtail): infraestrutura única e compartilhada — não faz sentido subir uma instância de Grafana separada para o ingestion.
3. **Plugar o ingestion na mesma instância**: adicionar Prometheus + RabbitMQ exporter como datasource no Grafana já existente.

## Dependência da Fase 10b do gateway (CI/CD)

A Fase 10b do `PLANO_GATEWAY_HTTP.md` define o pipeline de CI/CD (GitHub Actions + self-hosted runner + Docker) para o gateway. Os componentes deste plano entram no mesmo pipeline, em vez de terem um pipeline próprio:

- **IngestionService**, **ReadingWriterService** e **SensorSimulatorService** ganham estágios de build no `Dockerfile` multi-stage (ou Dockerfiles próprios), testados no mesmo `ci.yml` a cada PR.
- **RabbitMQ** entra como serviço de infraestrutura no compose (imagem oficial, sem build próprio), com config/provisionamento versionado (definições de fila, plugins management + Prometheus exporter habilitados).
- **Dashboards do Grafana** (painéis de fila/RabbitMQ) versionados como JSON e provisionados automaticamente no deploy, em vez de configurados manualmente na UI.
- O critério de aceite da Fase 10b ("merge na main → novo container em produção sem intervenção manual") passa a cobrir todo o pipeline de ingestão, não só o gateway.

---

## SensorSimulatorService (necessário enquanto o hardware não está pronto)

**Contexto:** o hardware físico dos sensores ainda não está finalizado, mas o restante da stack (IngestionService, fila, ReadingWriterService) precisa ser desenvolvido e testado de ponta a ponta. A solução é um simulador que se comporta exatamente como um device real do ponto de vista do IngestionService — o mesmo protocolo CoAP/DTLS, o mesmo formato de payload — de forma que o gateway/IngestionService seja totalmente transparente a estar falando com hardware real ou simulado.

**Configuração por instância simulada:**
- `sensorId` — identifica o sensor simulado (correlaciona com `SensorService`).
- `tipo de sensor` — ex: temperatura, umidade, pressão.
- `medida` — unidade de medida gerada (ex: °C, %, hPa).
- `período agendado` — intervalo entre envios, simulando o ciclo de wake/sleep do device real.
- `faixa de valores` — range (min/max) usado para gerar os valores simulados, possivelmente com variação aleatória/ruído para parecer mais realista.

**Responsabilidades:**
- Gerar valores dentro da faixa configurada, no período agendado.
- Enviar via CoAP/DTLS para o IngestionService, usando as mesmas credenciais (PSK/certificado) que um device real usaria.
- Permitir rodar múltiplas instâncias simultâneas (múltiplos sensores/devices simulados) para testar carga e concorrência no IngestionService.

**Configuração via banco (decisão):** o simulador lê seus parâmetros (`sensorId`, tipo, medida, período, faixa de valores) direto do cadastro real em `DeviceService`/`DeviceConfigService`/`SensorService`, em vez de arquivo local. Isso reforça a transparência do simulador — ele usa exatamente o mesmo cadastro que um device real usaria — e evita duplicar/dessincronizar configuração entre dois lugares.

**Pontos em aberto:**
- Definir se o simulador deve gerar cenários de falha propositais (payload fora da faixa, perda de pacote, device offline) para testar a robustez da validação no IngestionService.
