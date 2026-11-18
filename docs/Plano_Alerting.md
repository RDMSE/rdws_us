Data: 2026-07-09 (reduzido em 2026-11-18, RDWS-221)

# Plano de Alarmes (Alerting): fronteira e Camada 1

A plataforma não avalia alarmes. Detectar condições anormais (limites, correlação entre
sinais, tendências), registrar alarmes por fazenda e notificar alguém acontece fora deste
repositório, a partir das leituras que a plataforma entrega. Este documento guarda o que é
da plataforma nesse fluxo.

## Fronteira (decisão de 2026-11-14)

- **Device:** a estação detecta uma borda nos próprios sensores (`triggers[]` da config) e
  antecipa o envio. Isso não é um alarme (Camada 1, abaixo).
- **Plausibilidade física:** faixa por unidade, com a flag `out_of_range` na ingestão
  (RDWS-174). Marca a leitura; não é alarme, e quem avalia alarmes ignora leituras marcadas
  (`flags & 0x4000`).
- **Alarmes:** limites (inclusive os simples), correlação entre sinais, séries temporais,
  histerese e notificação. Fora deste repositório.

### Como um avaliador de alarmes recebe as leituras (RDWS-216)

Declara a própria fila durável (por exemplo `sensor_readings.alerting`) e a liga à exchange
fanout `rdws.sensor_readings` (no código, `AmqpConsumer` com
`rdws::amqp::kSensorReadingsExchange`). Não consome da `sensor_readings`: ali dois
consumidores dividiriam as mensagens em vez de cada um receber todas. Enquanto a fila dele
não existir, as leituras não chegam a ele; o registro no banco não é afetado, porque a
ingestion garante a fila do writer.

## Camada 1 — Device (edge triggers)

**Decisão (2026-11-14, RDWS-201, separação open-core):** não existe filtro de primeira ordem
no `ReadingWriterService` nem na ingestão. O desenho anterior (teste `valor > limite` no
caminho de escrita, publicando `reading.threshold_breach`) ficou sem motivo:

- Quem avalia alarmes recebe uma cópia de cada leitura pela fanout `rdws.sensor_readings`
  quase em tempo real, sem consulta extra: um teste no caminho de escrita não seria mais
  barato.
- Não há duas fontes de limites (config do device vs. regras de alarme) nem checagem
  duplicada.
- Alarmes ficam fora deste repositório; o caminho de escrita é daqui. Um teste no writer
  obrigaria a plataforma a conhecer o conceito de alarme, invertendo a dependência.

O que é a primeira camada, então, é o **device**: os `triggers[]` da config
(`Plano_Telemetria.md`, RDWS-172; `rise`/`fall` em janela, `gt`/`lt` com histerese) fazem a
estação antecipar o envio quando um sinal muda. O backend valida e entrega essa config; o
trigger não abre alarme nenhum. Os dados chegam mais cedo, e quem decide é quem avalia os
alarmes.

## Alertas de frota

Estação silenciosa (sem envio além de N × intervalo) e atraso de ingestão são de operação,
não do produtor (`Plano_Telemetria.md` D10): regra "Silent station" e painel "Ingestion
delay" no Grafana desde 2026-10-07. O canal de notificação de frota ainda não foi escolhido.
