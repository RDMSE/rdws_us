Data: 2026-07-09

# Plano de Alarmes (Alerting)

O `Plano_Ingestion.md` define como as leituras chegam até `sensor_readings`. Este documento cobre o que acontece a partir daí: como detectar condições anormais (limites excedidos, dewpoint, tendências) e gerar alarmes — habilitáveis por fazenda — sem acoplar essa lógica ao caminho de escrita das leituras.

---

## Visão geral do fluxo

```
[RabbitMQ] ──(mesmo stream do ReadingWriterService)──> [AlertingService] ──avalia regras──> [alarm_events] ──publica evento──> [NotificationService]
                                                              │
                                                    [PostgreSQL/TimescaleDB]
                                                    (janelas de série temporal
                                                     via query, sem estado em
                                                     memória)
```

Duas camadas, com responsabilidades e requisitos de latência diferentes — por isso não vivem no mesmo processo:

1. **Filtro de primeira ordem** — testes instantâneos sobre uma leitura isolada (`valor > limite`). Barato, sem estado, cabe no caminho de escrita.
2. **AlertingService** — correlação entre sinais (dewpoint precisa de temperatura *e* umidade juntas) e série temporal (tendência, janela deslizante). Precisa de estado que sobrevive entre leituras — não cabe no caminho síncrono de escrita.

---

## Camada 1 — Filtro de primeira ordem

- Roda dentro do `ReadingWriterService` (ou antes, no `IngestionService`), no momento em que a leitura já está sendo processada — sem round-trip extra.
- Testa a leitura isolada contra o(s) limite(s) simples configurado(s) pro sensor (min/max), lidos de `device_config` ou de `alarm_rules` (ver modelo de dados abaixo) — a decidir na implementação qual fonte é a autoritativa pra não duplicar configuração.
- Só decide "essa leitura merece atenção" — não decide se um alarme deve ser aberto/fechado (isso é responsabilidade da Camada 2, que trata histerese). Publica um evento leve (`reading.threshold_breach`) no barramento interno; não bloqueia nem atrasa a escrita da leitura em si.

## Camada 2 — AlertingService

**Decisão de design:** processo separado, consumindo o mesmo stream do RabbitMQ que o `ReadingWriterService` (fan-out — RabbitMQ já suporta múltiplos consumers/filas independentes do mesmo exchange), em paralelo, sem adicionar latência no caminho de escrita.

- **Sem estado em memória para série temporal.** `sensor_readings` já é TimescaleDB — janelas ("média dos últimos 15min", "tendência de subida") são queries janeladas direto no banco, não estado mantido pelo processo. Mais simples e sobrevive a restart do serviço sem perder contexto. Estado em memória só valeria a pena se a latência de detecção precisasse ser sub-segundo, o que não é o caso (leituras agendadas, não streaming de alta frequência — mesmo racional já usado em `Plano_Ingestion.md` pra escolher RabbitMQ).
- **Histerese obrigatória.** Sem isso, um valor oscilando em torno do limite dispara o mesmo alarme repetidamente. Resolvido no modelo de dados: um alarme só é reaberto se o anterior (mesma regra + mesmo sensor) já estiver `resolved`; ao encontrar uma leitura que não viola mais a condição, o `AlertingService` fecha (`resolved_at`) o `alarm_event` ativo correspondente.
- **Correlação entre sinais** (ex.: dewpoint = f(temperatura, umidade)) exige que o `AlertingService` busque as leituras mais recentes de sensores relacionados (mesmo `device_id`, tipos `temperature` + `humidity`) em vez de avaliar cada leitura isoladamente — é o motivo principal pelo qual isso não cabe na Camada 1.
- Habilitado/desabilitado por fazenda via `alarm_rules.enabled` (ver abaixo) — o serviço consulta as regras ativas da fazenda do device antes de avaliar qualquer condição.

## Modelo de dados

- **alarm_rules**
    - id : BIGINT (PK, auto-increment)
    - farm_id : BIGINT (FK → farms.id) NOT NULL — escopo mínimo de habilitação, conforme pedido ("habilitados ou não, dependendo de cada fazenda").
    - metric : ENUM('temperature', 'humidity', 'moisture', 'ph', 'luminosity', 'dewpoint', 'other') NOT NULL
    - condition : ENUM('gt', 'gte', 'lt', 'lte') NOT NULL
    - threshold : NUMERIC(12, 6) NOT NULL
    - window_minutes : INTEGER NULL — NULL = avalia leitura instantânea; preenchido = avalia média/tendência na janela (ex: 15).
    - severity : ENUM('warning', 'critical') NOT NULL DEFAULT 'warning' — decide o canal de notificação (ver `NotificationService`).
    - enabled : BOOLEAN NOT NULL DEFAULT true
    - created_at, updated_at, updated_by — mesmo padrão das demais tabelas.

- **alarm_events** *(append-only, mesmo padrão de `sensor_readings`, com duas exceções atualizáveis: `resolved_at` fecha o alarme, `acknowledged_at`/`acknowledged_by` registram reconhecimento humano — nenhuma das duas duplica linha)*
    - id : BIGINT (PK, auto-increment)
    - rule_id : BIGINT (FK → alarm_rules.id) NOT NULL
    - sensor_id : BIGINT (FK → sensors.id) NOT NULL — sensor que disparou (ou o principal, no caso de métricas derivadas como dewpoint).
    - triggered_at : TIMESTAMPTZ NOT NULL
    - resolved_at : TIMESTAMPTZ NULL — NULL = alarme ainda ativo (condição continua violada).
    - acknowledged_at : TIMESTAMPTZ NULL — NULL = ninguém reconheceu ainda; para o escalonamento quando preenchido.
    - acknowledged_by : BIGINT (FK → users.id) NULL
    - triggering_value : NUMERIC(12, 6) NOT NULL
    - status : ENUM('active', 'acknowledged', 'resolved') NOT NULL DEFAULT 'active'

> Índice único parcial recomendado: `(rule_id, sensor_id) WHERE status IN ('active', 'acknowledged')` — reforça a histerese no próprio banco (não é possível ter dois alarmes em aberto pra mesma regra+sensor simultaneamente).

## NotificationService

Não é "escreve no banco e esquece" — um sistema de alarme de verdade precisa fechar o loop até uma pessoa agir, e fazer isso sem gastar dinheiro à toa (SMS é cobrado por mensagem).

- **Reconhecimento (acknowledgement), não só disparo.** `alarm_events.acknowledged_at`/`acknowledged_by` (ver modelo acima) — o escalonamento (próximo item) para assim que alguém reconhece. Sem isso o sistema manda SMS pra sempre até o sensor voltar ao normal, mesmo que alguém já esteja resolvendo.
- **Escalonamento por tempo, não fire-and-forget.** Ordem sugerida: push/e-mail imediato → se não reconhecido em N minutos, escala pra SMS → se ainda não reconhecido, escala pro próximo contato cadastrado da fazenda. Resolve dois problemas de uma vez: custo (SMS só quando realmente importa) e efetividade (dá tempo de alguém já estar resolvendo antes do SMS disparar). `N` e a cadeia de contatos são configuráveis por fazenda, não hardcoded.
- **Severidade decide o canal inicial.** `alarm_rules.severity = critical` pula direto pra SMS/ligação; `warning` começa em push/e-mail e só escala se ninguém reconhecer.
- **Abstração de provedor.** SMS custa por mensagem (Twilio, AWS SNS, ou provedor nacional tipo Zenvia/TotalVoice) — uma interface (`INotificationChannel`) permite trocar/negociar provedor sem reescrever a lógica de escalonamento, e centraliza onde rastrear custo.
- **Custo como métrica, não detalhe escondido.** Volume de notificações (por canal, por fazenda) exposto no mesmo stack Prometheus/Grafana já montado (`Plano_Deployment.md` §3) — dá pra ver logo se uma fazenda está gerando volume anormal de SMS (sensor com defeito piscando entre normal/alarme, por exemplo) antes da fatura chegar.
- **Silenciar durante manutenção, de graça.** `devices.status` já tem o valor `maintenance` no schema atual (`Plano_DB_IOT_Sensors.md`) — o `AlertingService` deveria pular avaliação (ou pelo menos suprimir notificação, mantendo o registro em `alarm_events` pra histórico) pra devices nesse estado. Sem isso, todo desligamento pra manutenção vira alarme de "sensor não responde", e as pessoas aprendem a ignorar alarmes — o pior resultado possível pra um sistema desses.
- **Cooldown por regra+sensor, além da histerese.** Histerese evita duplicar enquanto o alarme está aberto; cooldown evita reenviar notificação se o mesmo alarme fechar e reabrir rápido demais (ex.: sensor com ruído oscilando perto do limite) — janela mínima entre notificações da mesma regra+sensor, mesmo que sejam `alarm_events` diferentes.
- Consumidor separado do evento publicado quando um `alarm_event` muda de estado (`alarm.triggered` / `alarm.acknowledged` / `alarm.resolved`) — mesmo padrão de desacoplamento do `PersistenceService` (`Plano_Gateway_HTTP.md` Fase 10a): o `AlertingService` não sabe nem se importa como o alarme é entregue.
- Templates de mensagem, cadastro de contatos por fazenda e detalhamento de integração com o provedor de SMS ficam pra quando a implementação começar de fato — o essencial aqui é o desenho de escalonamento/custo/silenciamento, que afeta o modelo de dados acima.

---

## Pontos em aberto

- Fonte autoritativa dos limites simples da Camada 1: `device_config` (já existe, mas é por device) vs. `alarm_rules` (novo, por farm) — evitar duas fontes de verdade pro mesmo tipo de limite.
- `window_minutes` cobre "média na janela"; tendência de subida/descida (derivada, não média) fica de fora do desenho inicial — avaliar se é necessário antes de implementar ou se entra numa v2.
- Tempo de escalonamento (`N` minutos até virar SMS) e cadeia de contatos por fazenda: onde vive essa config — nova tabela (`farm_contacts`?) ou reaproveita `users` com algum vínculo à fazenda?
- Escolha do provedor de SMS (custo, cobertura nacional, API) — decisão de implementação, não de arquitetura.
- Onde o `AlertingService` roda no gateway existente: como capability registrada (`alerting.evaluate` chamado por algo) ou como worker puro sem capability HTTP, no mesmo espírito do `ReadingWriterService`? Tende a ser worker puro, já que não responde a nenhuma chamada síncrona.

## Ordem de implementação e dependências

Depende do `Plano_Ingestion.md` estar funcionando ponta a ponta primeiro — sem leituras reais fluindo pelo RabbitMQ, não há o que avaliar. Ordem sugerida:

1. Pipeline de ingestão estável (`Plano_Ingestion.md`, já com RabbitMQ implementado — hoje adiado, ver `Plano_Deployment.md` §6 passo 5).
2. Migration Flyway para `alarm_rules` e `alarm_events`.
3. Filtro de primeira ordem no `ReadingWriterService` (mais simples, valida o desenho de configuração por fazenda antes do serviço separado existir).
4. `AlertingService` (consumo do RabbitMQ, queries janeladas, histerese, silenciamento por manutenção) — dockerizado no mesmo pipeline de CI/CD já estabelecido (`Plano_Gateway_HTTP.md` Fase 10b), mesmo padrão dos demais serviços.
5. `NotificationService` (escalonamento, reconhecimento, abstração de provedor) — plano próprio com mais detalhe quando chegar a vez; o desenho de dados/fluxo já está fixado aqui.
