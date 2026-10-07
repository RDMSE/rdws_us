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

**Nota (mecanismo de leitura do limite, 2026-07-12):** a premissa de "barato, sem
round-trip extra" exige um desenho explícito — sem ele, a implementação óbvia é uma
query por leitura processada, o que contradiz a própria premissa. Mesmo padrão já
validado em `Plano_DeviceCredentials.md` (§5) para o mesmo tipo de problema (valor
precisa estar disponível rápido, dentro de um consumer, sem round-trip por evento):
cache em memória no `IngestionService`/`ReadingWriterService`, carregado no startup, com
invalidação pontual via evento (`alarm_rule.changed`, publicado quando um limite muda) em
vez de recarregar tudo ou fazer polling. Assim que a fonte autoritativa for decidida
(`device_config` vs. `alarm_rules`), o cache espelha essa fonte; a decisão de qual é
autoritativa continua em aberto (ver "Pontos em aberto"), mas o mecanismo de acesso
barato já não precisa ser reinventado.
- Só decide "essa leitura merece atenção" — não decide se um alarme deve ser aberto/fechado (isso é responsabilidade da Camada 2, que trata histerese). Publica um evento leve (`reading.threshold_breach`) sem bloquear nem atrasar a escrita da leitura em si.

**Nota (transporte do evento, 2026-07-12):** "barramento interno" no parágrafo acima
não pode ser o `EventBus` do `Plano_Gateway_HTTP.md` (Fase 7) — aquele é em memória, por
processo, single-instância (mesma limitação já registrada na Fase 14, "agregação de
métricas entre instâncias" como problema em aberto). A Camada 1 roda dentro do
`IngestionService`/`ReadingWriterService`, processos separados do gateway, sem acesso a
esse `EventBus`. Duas questões ficam em aberto antes de implementar:
- **Transporte real**: outra fila/routing key no mesmo exchange do RabbitMQ (natural,
  já que a Camada 1 já vive dentro de um consumer desse broker), ou mecanismo à parte?
- **Consumidor**: se for o `AlertingService`, ele já recebe o stream de leituras cru via
  Camada 2 — testar o limite simples de novo ali duplica o trabalho da Camada 1. Se o
  consumidor é outra coisa (ex. um painel de "atenção" com latência menor que o ciclo do
  `AlertingService`), isso precisa ser nomeado — hoje o evento é descrito sem canal nem
  consumidor concretos. Ver também "Pontos em aberto".

## Camada 2 — AlertingService

**Decisão de design:** processo separado, consumindo o mesmo stream do RabbitMQ que o `ReadingWriterService` (fan-out — RabbitMQ já suporta múltiplos consumers/filas independentes do mesmo exchange), em paralelo, sem adicionar latência no caminho de escrita.

- **Sem estado em memória para série temporal.** Assumindo `sensor_readings` como
  hypertable TimescaleDB — **nota: isso ainda não é decisão fechada.** `Plano_DB_IOT_Sensors.md`
  lista TimescaleDB como "alternativa recomendada" ao particionamento manual, não como
  compromisso; o SQL de exemplo lá usa `PARTITION BY RANGE` nativo do Postgres, sem
  `create_hypertable`. Se as queries janeladas abaixo (`time_bucket`, médias móveis)
  dependem de funções nativas do TimescaleDB, essa decisão precisa ser fechada em
  `Plano_DB_IOT_Sensors.md` antes deste plano avançar para implementação — caso contrário
  as mesmas janelas têm que ser reescritas com funções de window do Postgres puro
  (`AVG(...) OVER (...)`), o que é viável, só não é o que o texto abaixo assume. Janelas
  ("média dos últimos 15min", "tendência de subida") são queries janeladas direto no
  banco, não estado mantido pelo processo. Mais simples e sobrevive a restart do serviço sem perder contexto. Estado em memória só valeria a pena se a latência de detecção precisasse ser sub-segundo, o que não é o caso (leituras agendadas, não streaming de alta frequência — mesmo racional já usado em `Plano_Ingestion.md` pra escolher RabbitMQ).
- **Histerese obrigatória.** Sem isso, um valor oscilando em torno do limite dispara o mesmo alarme repetidamente. Resolvido no modelo de dados: um alarme só é reaberto se o anterior (mesma regra + mesmo sensor) já estiver `resolved`; ao encontrar uma leitura que não viola mais a condição, o `AlertingService` fecha (`resolved_at`) o `alarm_event` ativo correspondente.
- **Correlação entre sinais** (ex.: dewpoint = f(temperatura, umidade)) exige que o `AlertingService` busque as leituras mais recentes de sensores relacionados (mesmo `device_id`, tipos `temperature` + `humidity`) em vez de avaliar cada leitura isoladamente — é o motivo principal pelo qual isso não cabe na Camada 1.
- Habilitado/desabilitado por fazenda via `alarm_rules.enabled` (ver abaixo) — o serviço consulta as regras ativas da fazenda do device antes de avaliar qualquer condição.

## Modelo de dados

- **alarm_rules**
    - id : BIGINT (PK, auto-increment)
    - farm_id : BIGINT (FK → farms.id) NOT NULL — escopo mínimo de habilitação, conforme pedido ("habilitados ou não, dependendo de cada fazenda").
    - metric : ENUM('temperature', 'humidity', 'moisture', 'ph', 'luminosity', 'dewpoint', 'other') NOT NULL
      - **Nota (2026-10-07):** falta `pressure` (o tipo existe em `sensors` desde a V9) e
        faltam os índices derivados (`vpd`, `delta_t`, `pressure_tendency_3h`). Ver
        "Alertas sobre índices derivados".
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

**Decisão (regra desabilitada com alarme ativo, 2026-07-12):** `alarm_rules.enabled = false`
fecha automaticamente qualquer `alarm_event` `active`/`acknowledged` daquela regra
(`resolved_at = now()`, `status = 'resolved'`) no momento em que a regra é desabilitada —
não espera o próximo ciclo do `AlertingService` nem deixa o alarme órfão (aberto pra
sempre, já que a avaliação que o fecharia parou de rodar). Executado no mesmo handler que
seta `enabled = false` (capability de update de `alarm_rules`), não no `AlertingService` —
evita depender do timing do próximo ciclo de avaliação para uma limpeza que deveria ser
imediata. Sem essa regra, desabilitar uma regra problemática (ex.: sensor com ruído
gerando alarme falso) deixaria lixo permanente em `alarm_events`.

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

## Alertas sobre índices derivados (2026-10-07)

Os índices são calculados conforme `Plano_Indices_Derivados.md` (views instantâneas,
`daily_weather`, `wetness_periods`). Esta seção define quais viram alerta e o que isso
muda no modelo acima.

### Alertas propostos

| Alerta | Base | Tipo de regra | Severidade sugerida |
|---|---|---|---|
| Risco de geada (preditivo) | Td no fim da tarde, taxa de resfriamento após o pôr do sol, T | composta | `warning` → `critical` |
| Queda barométrica rápida | tendência 3 h (dados a tempo pelo gatilho `fall` do firmware) | limiar simples sobre índice | `warning` |
| Risco de doença fúngica | período de molhamento (duração + T média) | por período | `warning` |
| DPV fora da faixa | DPV sustentado por N horas | limiar com `window_minutes` | `warning` |

- **Risco de geada.** Não é um limiar único: combina o Td no fim da tarde (o ponto de
  orvalho é o piso aproximado do resfriamento noturno), a taxa de queda de T nas primeiras
  horas depois do pôr do sol (calculado pela latitude e dia do ano) e a própria T. "Noite
  limpa" não é mensurável sem sensor de radiação; a taxa de resfriamento é o substituto.
  Os limiares precisam considerar a altura do sensor: em noite de radiação, folha e relva
  ficam alguns graus abaixo do ar a 1,5–2 m.
- **Dados a tempo: edge trigger em estágios.** Um alerta preditivo só funciona se os
  dados do fim da tarde e do início da noite chegarem ao servidor nesse horário. Com
  1 uplink/dia, quem garante isso é o edge trigger, usado como **pré-aviso** e não só como
  detecção de geada:
  - O firmware não avalia risco de geada (sem correlação entre sinais). Ele só usa
    limiares de T em estágios, várias regras sobre `temp` em `triggers[]`. Exemplo:
    `T < 5 °C` ("noite que merece atenção") e `T < 2 °C` ("geada se aproximando").
  - Cada disparo descarrega o backlog pendente, então o servidor recebe a série da tarde
    e da noite até aquele momento. Com ela, o `AlertingService` calcula o Td no fim da
    tarde e a taxa de resfriamento, e decide o alerta.
  - O segundo estágio traz dados novos mais perto do evento. Como o latch só rearma
    quando a condição termina, sem ele o servidor ficaria sem dados entre o primeiro
    disparo e a manhã.
  - Custo: na estação fria, o primeiro estágio pode disparar quase toda noite. São um ou
    dois uplinks na entrada e um na saída pela manhã, justamente no período em que o
    alerta importa.
  - Limite: o pré-aviso só chega quando T cruza o primeiro estágio, em geral algumas
    horas depois do pôr do sol. Para antecipar, o backend agenda um envio extra perto do
    pôr do sol no período de geada (política `sunset_frost` em `extra_uplinks_utc`,
    `rdws_weather_node/docs/Plano_Firmware_WeatherNode.md` §3). A noite fica: envio no pôr do sol (Td do fim da
    tarde) → estágio de 5 °C (taxa de resfriamento) → estágio de 2 °C (geada próxima) →
    saída da violação pela manhã (fecha o alarme).
- **Queda barométrica: mesmo raciocínio da geada.** Com 1 uplink/dia a tendência de 3 h
  chegaria até 24 h depois. Uma regra `{"chan":"press","op":"fall","thr":360,"win_s":10800}`
  em `triggers[]` (`rdws_weather_node/docs/Plano_Firmware_WeatherNode.md` §4) descarrega o backlog quando a
  queda passa do limiar, e o `AlertingService` confirma pela view de tendência.
  O limiar do device e o da regra de alarme podem ser iguais ou o do device um pouco menor
  (pré-aviso). Abaixo de ~3 hPa a maré barométrica diária dispara a regra todo dia.
- **Delta T não é alarme.** É informação de janela de pulverização para o dashboard
  (`Plano_Indices_Derivados.md` D8). Uma notificação "boa janela agora" seria outra
  categoria (informativa, opt-in), fora deste plano.
- **Acumulados (GDD, horas de frio) também não são alarme.** Atingir um estágio
  fenológico ou a necessidade de frio de uma cultivar é aviso informativo. Mesmo
  tratamento: categoria própria, depois.

### Impacto no modelo de dados

- `alarm_rules.metric`: incluir `pressure`, `vpd`, `delta_t` e `pressure_tendency_3h`.
  Essas cabem no formato atual (`metric`, `condition`, `threshold`, `window_minutes`).
- Geada e doença fúngica não cabem em `metric` + `condition` + `threshold`. Proposta: uma
  coluna `rule_type` (`threshold` por padrão, `frost_risk`, `fungal_risk`) e uma coluna
  `params JSONB` validada por JSON Schema por tipo, no mesmo padrão de
  `device_configurations.config`. As colunas de limiar continuam valendo para o tipo
  `threshold`.
- `alarm_events.sensor_id` para métricas derivadas (proposta, resolve o ponto em aberto):
  o sensor da grandeza dominante. Temperatura para `dewpoint`, `vpd`, `delta_t` e
  `frost_risk`; pressão para `pressure_tendency_3h`; umidade para `fungal_risk`.

### Avaliação por (device, timestamp) (decisão 2026-10-07)

As três grandezas de um mesmo instante podem chegar em mensagens diferentes: um POST leva
até 10 registros, então um lote pode separar T, UR e P do mesmo fim de janela. Por isso as
regras sobre índices derivados não são avaliadas por leitura isolada:

- Cada leitura que chega identifica um (device, timestamp). O `AlertingService` consulta
  `weather_samples` (`Plano_Indices_Derivados.md` D1) para esse par e só avalia quando as
  grandezas de que a regra precisa existirem. Se faltar alguma, não faz nada: a leitura
  que completar o conjunto dispara a avaliação.
- Uma leitura que chega depois reavalia o mesmo par. Com a histerese e o índice único de
  `alarm_events`, reavaliar é idempotente.
- Regras de limiar sobre uma grandeza bruta (`temperature`, `pressure`) continuam sendo
  avaliadas por leitura.

### Leituras atrasadas

O `AlertingService` avalia a leitura quando ela chega, não quando foi medida. Um backlog
de vários dias (estação offline, 1 uplink/dia) pode conter eventos que começaram e
terminaram há muito tempo. Proposta: o `alarm_event` é registrado com `triggered_at` e
`resolved_at` históricos (o histórico fica correto), mas só gera notificação se ainda
estiver ativo na leitura mais recente daquele device, ou se tiver começado há menos de um
limite configurável. "Fim do lote" não serviria: o `AlertingService` vê mensagens, não
ciclos de uplink (decidido em 2026-10-07).
Notificar uma geada que acabou ontem só gera ruído.

---

## Pontos em aberto

- Evento `alarm_rule.changed` (invalidação do cache da Camada 1) — nome/payload/exchange ainda não definidos, mesmo racional do `device_credential.changed` em `Plano_DeviceCredentials.md`.
- Transporte e consumidor do evento `reading.threshold_breach` da Camada 1 — não pode ser o `EventBus` do gateway (single-instância, por processo); definir se é fila/routing key própria no RabbitMQ e quem consome sem duplicar a avaliação já feita pela Camada 2 (ver nota na seção da Camada 1).
- Se `sensor_readings` realmente será hypertable TimescaleDB (`create_hypertable`) ou particionamento nativo do Postgres — adiado em 2026-10-07 (`Plano_Telemetria.md` Fase 3, R3: tabela simples até o volume justificar); as queries janeladas deste plano devem funcionar sobre a tabela simples primeiro.
- ~~Qual sensor é "o principal" em `alarm_events.sensor_id` para métricas derivadas~~ — proposta em "Alertas sobre índices derivados" (2026-10-07): o sensor da grandeza dominante. Confirmar.
- Fonte autoritativa dos limites simples da Camada 1: `device_config` (já existe, mas é por device) vs. `alarm_rules` (novo, por farm) — evitar duas fontes de verdade pro mesmo tipo de limite.
- `window_minutes` cobre "média na janela"; tendência de subida/descida (derivada, não média) fica de fora do desenho inicial — avaliar se é necessário antes de implementar ou se entra numa v2.
- Tempo de escalonamento (`N` minutos até virar SMS) e cadeia de contatos por fazenda: onde vive essa config — nova tabela (`farm_contacts`?) ou reaproveita `users` com algum vínculo à fazenda?
- Escolha do provedor de SMS (custo, cobertura nacional, API) — decisão de implementação, não de arquitetura.
- **Alertas de frota ficam fora deste plano** (`Plano_Telemetria.md` D10): estação
  silenciosa (sem envio além de N × intervalo) e atraso de ingestão são de
  operação/manutenção, não do produtor, e têm canal de notificação separado. Implementados
  no Grafana em 2026-10-07 (regra "Silent station", painel "Ingestion delay"); o canal de
  notificação de frota ainda não foi escolhido. Bateria e
  painel solar continuam em `sensor_readings` (D6) justamente para serem avaliados aqui.
- Onde o `AlertingService` roda no gateway existente: como capability registrada (`alerting.evaluate` chamado por algo) ou como worker puro sem capability HTTP, no mesmo espírito do `ReadingWriterService`? Tende a ser worker puro, já que não responde a nenhuma chamada síncrona.
- `rule_type` + `params JSONB` para regras compostas (geada, doença fúngica): confirmar o
  desenho e definir o schema de `params` de cada tipo.
- Parâmetros iniciais do alerta de geada (Td de referência, taxa de resfriamento, T) e do
  alerta de doença (duração mínima do período e faixa de T média), por cultura ou por
  fazenda. Validar com agrônomo.
- Limite de idade para notificar alarmes vindos de leituras atrasadas.
- Limiares dos estágios de edge trigger para geada (ex.: 5 °C e 2 °C), offset do envio
  no pôr do sol e onde vivem as datas do período de geada de cada fazenda.

## Ordem de implementação e dependências

Depende do `Plano_Ingestion.md` estar funcionando ponta a ponta primeiro — sem leituras reais fluindo pelo RabbitMQ, não há o que avaliar. Ordem sugerida:

1. ✅ Pipeline de ingestão estável (`Plano_Ingestion.md`): RabbitMQ, `IngestionService` e
   `ReadingWriterService` rodando em QA, com SenML, desde 2026-10 (`Plano_Telemetria.md`).
2. Migration Flyway para `alarm_rules` e `alarm_events`.
3. Filtro de primeira ordem no `ReadingWriterService` (mais simples, valida o desenho de configuração por fazenda antes do serviço separado existir).
4. `AlertingService` (consumo do RabbitMQ, queries janeladas, histerese, silenciamento por manutenção) — dockerizado no mesmo pipeline de CI/CD já estabelecido (`Plano_Gateway_HTTP.md` Fase 10b), mesmo padrão dos demais serviços.
5. Alertas sobre índices derivados (seção própria acima), depois das views e de
   `daily_weather`/`wetness_periods` do `Plano_Indices_Derivados.md`.
6. `NotificationService` (escalonamento, reconhecimento, abstração de provedor) — plano próprio com mais detalhe quando chegar a vez; o desenho de dados/fluxo já está fixado aqui.
