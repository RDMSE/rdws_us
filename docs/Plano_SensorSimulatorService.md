Data: 2026-07-05

# Plano de desenvolvimento da ferramenta de apoio `Sensor Simulator Service`

Este plano descreve a ferramenta `Sensor Simulator Service`, que deverá ser o ponto de
apoio principal para o desenvolvimento e tracking da camada de entrada de dados para o
`rdws`. Aplicação completamente separada dos demais serviços (ver `Plano_Deployment.md`,
§1 e §6) — no máximo com conexão direta ao banco.

----

## Visão geral do fluxo

```
[Sensor Simulator Service] --lê config do banco--> gera dados simulados
  --persiste em arquivo--> quando chega o horário agendado --> abre conexão CoAP --> envia dados
```

1. **Sensor Simulator Service** lê os dados de sensores e dispositivos no banco de
   dados. Configuração via banco (decisão já tomada em `Plano_Ingestion.md`): o
   simulador usa exatamente o mesmo cadastro que um device real usaria, em vez de
   arquivo local.

2. Com as configurações obtidas do banco, o serviço começa a gerar os dados como se
   fosse um lote de sensores reais. Nesse ponto o serviço sabe quais sensores deve
   simular e a quais devices esses sensores estão atrelados, e gera os valores de
   acordo com a configuração (tipo, unidade, faixa de valores) — salvando em arquivo
   `<device_id><sensor_id>.data` (JSON) em vez de manter só em memória. **Motivo**: o
   simulador precisa rodar por longos períodos, simulando um sistema real de fato — um
   restart do processo não deve perder o lote de leituras já gerado e ainda não enviado.

3. Quando o horário local se torna igual ao horário agendado para o dispositivo
   enviar os dados: a geração de novas leituras é **pausada** (lock via mutex sobre o
   arquivo daquele device/sensor — há pelo menos duas threads acessando o mesmo arquivo,
   a que gera e a que envia) e o processo de envio começa. O serviço abre uma conexão
   CoAP/DTLS com o `IngestionService`, usando as credenciais (PSK/certificado) daquele
   device. Para cada dispositivo, o `Sensor Simulator Service` abre uma nova conexão,
   para simular como seria em um ambiente real.

4. `Sensor Simulator Service` envia os dados; ao terminar, apaga/limpa o arquivo e
   libera o lock, reiniciando o ciclo de geração (volta ao passo 2) — evita a necessidade
   de controle de idempotência mais sofisticado (offset, marca de "enviado"), já que o
   arquivo só existe entre uma geração e o envio seguinte.

**Agendamento**: cada instância do `Sensor Simulator Service` controla seu próprio timer
— não há um processo único centralizando o agendamento de múltiplos devices. Simulação
de vários devices roda como várias instâncias, cada uma responsável pelo seu ciclo
gerar → pausar → enviar → limpar.

## Como identificar um device/sensor como simulado

`device_type` e `sensor_type` continuam representando o tipo real do equipamento
(`weather_station`, `single_sensor`, `gateway` etc.) — misturar "é simulado?" dentro
desses enums forçaria duplicar cada tipo real como uma variante de simulação. As duas
perguntas são ortogonais: que tipo de device é, e se ele é simulado ou real.

Em vez disso, um flag próprio (`is_simulated BOOLEAN`, em `devices`) marca a origem do
device sem afetar seu tipo. O `Sensor Simulator Service` filtra por esse flag ao carregar
sua lista de simulação.

**Sem flag duplicado em `sensors`**: cada sensor já pertence a um device (FK
`device_id`), então "esse sensor é simulado?" é derivado via join com `devices`, não uma
segunda fonte de verdade:

```sql
SELECT s.* FROM sensors s
JOIN devices d ON d.id = s.device_id
WHERE d.is_simulated = true;
```

Um device de simulação não faz sentido ter sensor real atrelado (e vice-versa) — a
simulação é sempre do device inteiro, com todos os seus sensores juntos. Evita manter
duas fontes de verdade sincronizadas (o risco de um sensor ficar com status inconsistente
do device pai).

**`is_simulated` é imutável em produção**: a coluna é definida na criação do device e
não pode ser alterada depois (sem endpoint de update para esse campo em produção) — o
JSON de criação é validado contra schema (valijson, mesmo padrão já usado no projeto em
`src/shared/validator/`), garantindo que o valor só entra do jeito certo desde o início.

## Interface de controle (escopo e disparo manual)

O serviço continua headless e de longa duração (roda sozinho, sem depender de interface
gráfica pra existir) — mas expõe uma pequena **API HTTP de controle** (mesmo padrão do
resto do projeto, via `cpp-httplib`), para disparo manual fora do horário agendado.

- **Restringir o escopo a um único device**: resolvido na **inicialização**, via
  parâmetro de CLI (ex. `--device-id X`) — coerente com a decisão de agendamento (cada
  instância controla seu próprio timer/device). Não precisa de endpoint em runtime para
  isso.
- **Disparo manual fora do horário agendado**: um endpoint que força o envio imediato
  das leituras de um device específico, sem esperar o agendamento.

Endpoint proposto:

| Método | Path | Descrição |
|---|---|---|
| `POST` | `/simulate/{device_id}/trigger` | Dispara o envio imediato das leituras daquele device, fora do agendamento. |

Sem necessidade de aplicação Qt: uma **nova coleção no Bruno** (mesmo padrão das já
existentes em `bruno/IoT Sensor API/`) cobre a operação manual desses endpoints,
reaproveitando a ferramenta que já é usada pro resto da API.

## Pontos em aberto

- **Provisionamento de credenciais DTLS** (PSK/certificado) por device simulado —
  resolvido em `Plano_DeviceCredentials.md`. O detalhamento de implementação (migrations,
  capabilities `device_credential.*`, `is_simulated` em `devices`, escolha do libcoap e
  desenho do `SensorSimulatorService`) está em
  `Plano_SensorSimulatorService_Implementacao.md`.
- Cenários de falha propositais (payload fora da faixa, perda de pacote, device offline)
  para testar a robustez do `IngestionService` — também já listado no `Plano_Ingestion.md`.
