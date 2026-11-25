Data: 2026-11-25

# Plano de OTA (atualização de firmware pela rede)

Como uma estação recebe, valida e adota uma imagem nova sem cabo, e como o servidor sabe quem roda o
quê. Cobre os dois repositórios: `rdws_us` (registro, entrega, acompanhamento) e
`rdws_weather_node` (download, troca, rollback).

**Estado:** decisões aceitas em 2026-11-25; fase 0 feita (RDWS-WE-016). Próxima: fase 1.

## Decisões (2026-11-25)

O motivo de cada uma está na seção indicada.

| # | Pergunta | Decisão | Seção |
|---|---|---|---|
| O1 | Onde ficam as imagens? | Arquivo em volume do servidor; metadados no Postgres | 3 |
| O2 | Quem entrega os blocos? | Processo novo (`firmware_service`), porta CoAP/DTLS própria | 4 |
| O3 | Como o device fica sabendo? | Oferta no `2.04`, só a quem declara suporte | 5 |
| O4 | Como o alvo é escolhido? | Por device, manual, só admin | 3 |
| O5 | Como se evita o laço baixar → reverter → baixar? | Device memoriza e declara a imagem rejeitada; servidor não a oferece | 6 |
| O6 | Quando o device baixa e reinicia? | Depois de enviar os dados; para se um edge trigger disparar; reinicia assim que verificar | 6 |
| O7 | Como se impede imagem da placa errada? | Classe da placa na imagem, conferida pelo device e pelo MCUboot | 7 |
| O8 | Como a imagem entra no registro? | Script na máquina de desenvolvimento; a chave nunca vai para o servidor | 3 |
| O9 | A estação 12 recebe a variante de bancada? | Sim, até haver release; a variante fica registrada | 3 |
| O10 | O download tem limite de tempo por ciclo? | Sim, configurável; sem limite no Wi-Fi, ligado no NB-IoT | 6 |

**O7 tem prazo:** o MCUboot não se atualiza em campo, então a conferência dentro dele entra na fase 0,
antes de a primeira placa sair para um lugar sem cabo.

## 1. O que já existe

| Peça | Situação | Onde |
|---|---|---|
| MCUboot: swap, confirmação, rollback, assinatura ECDSA-P256 | Validado na estação 12 (2026-11-25) | RDWS-WE-012 |
| Imagem em teste até o primeiro `2.04` de leituras; 3 h de limite | Validado | RDWS-WE-012 |
| Laço de boot da imagem confirmada | Validado | RDWS-WE-014 |
| `slot1`: 864 KB na QSPI; imagem atual de ~274 KB | Medido | RDWS-WE-012 |
| `fw_version` e `fw_build` (8 bytes do build ID) a cada ciclo, em `device_telemetry` | Em uso | `Plano_Telemetria.md` D2 |
| Corpo no `2.04` quando a config mudou | Em uso | `Plano_Telemetria.md` D11 |
| PSK por device, cache por `device_credential.list_active` | Em uso | `Plano_DeviceCredentials.md` |

Falta: guardar imagens, dizer ao device qual é a dele, entregá-la, e o device baixá-la. Hoje o
`scripts/slot1_load.sh` e o `img try` fazem esse papel pelo cabo.

### Lições que restringem o desenho

- **RDWS-157:** uma operação longa na thread CoAP única da ingestão atrasou os envios da estação em
  cerca de um dia. Nenhum byte de imagem passa por essa thread.
- **Trailer do slot0:** gravar por cabo com uma imagem em teste faz o MCUboot reverter no boot
  seguinte. A documentação de bancada já avisa.
- **QSPI compartilhada:** `slot1` e o LittleFS dos registros dividem a mesma memória. A aplicação
  apaga em setores de 4 KB dentro da partição; nunca além de `slot1`.

## 2. Visão geral

```
 máquina de dev            servidor                                   estação
 ──────────────            ────────                                   ───────
 west build + assina
 fw_publish ─────────────▶ registro (Postgres) + arquivo (volume)
                           admin escolhe o alvo do device
                           ingestion ◀── pack com fw_build, ota ───── ciclo de envio
                                     ─── 2.04 { fw: oferta } ───────▶ guarda a oferta
                           firmware_service ◀── GET bloco N ───────── baixa para o slot1
                                            ─── bloco N ────────────▶ confere SHA-256
                                                                      img try, reinicia
                                                                      MCUboot: swap, em teste
                           ingestion ◀── pack com fw_build novo ───── 2.04 confirma
```

## 3. Registro de imagens e alvo por device (rdws_us)

**O1.** O arquivo assinado (`zephyr.signed.bin`) fica num volume, nomeado pelo SHA-256. O Postgres
guarda só os metadados. Imagens de ~274 KB não justificam o banco, e o processo que as entrega lê do
disco.

```
firmware_images
  id, public_id, build_id (16 hex, único), sha256, size, board_class,
  fw_version, variant ('bench' | 'field'), notes, created_at, created_by, withdrawn_at

device_firmware_targets
  device_id (PK), image_id, set_at, set_by
```

Um alvo retirado (`withdrawn_at`) deixa de ser oferecido, e as imagens já instaladas não mudam.

**O4.** O alvo é por device e manual. Sem grupos nem rollout automático: com poucas estações, escolher
uma a uma é mais seguro. Um grupo canário vem depois, sobre a mesma tabela.

**O8.** A imagem é assinada na máquina que a compila. O servidor recebe um arquivo pronto e nunca a
chave. `scripts/fw_publish.sh` (no repositório do firmware):

1. confere a assinatura com a chave pública;
2. lê build ID, versão, tamanho e classe da placa do próprio artefato;
3. copia para o volume do servidor e chama a rota de registro.

A rota de registro recalcula o SHA-256 do arquivo em disco e recusa se não bater. O upload não passa
pelo gateway, porque ~274 KB em JSON pelo broker não fazem sentido.

**O9.** A variante (`bench` ou `field`) é registrada e mostrada. A de bancada traz comandos destrutivos
(`rdws fault`, `img erase`), e quem decide o alvo precisa ver isso.

### REST (admin)

| Rota | Efeito |
|---|---|
| `POST /firmware-images` | Registra uma imagem já copiada |
| `GET /firmware-images` | Lista |
| `PUT /devices/{id}/firmware-target` | Define o alvo; `null` remove |
| `GET /devices/{id}/firmware-status` | Imagem declarada, alvo e estado |

Estado derivado do que a estação declara, nunca gravado à mão: `none` (sem alvo), `pending` (alvo
diferente do declarado), `downloading`, `rejected` (o device declarou rejeição deste alvo),
`applied` (declara o build do alvo).

## 4. Entrega dos blocos (rdws_us)

**O2.** Um serviço novo, `firmware_service`, com seu processo e sua porta UDP (ex.: 5690; a 5685 já é o
relay de QA na VPS). Motivos para não usar a ingestão:

- o contexto libcoap não é seguro entre threads, e uma porta é um contexto;
- uma falha aqui não pode parar a coleta;
- no teto de memória da VPS, dá para limitar este processo à parte.

Contrato:

- `GET /fw` com Block2 (RFC 7959). O device não escolhe a imagem: o serviço entrega **o alvo do device
  autenticado pela PSK**. Sem alvo: `4.04`.
- `ETag` com o início do SHA-256. Se o alvo mudar no meio do download, o `ETag` muda, e o device
  recomeça do zero.
- Bloco de até 1024 bytes; o device pode pedir menos.
- A PSK é a mesma da ingestão, pela mesma capability, com o mesmo cache.
- Uma sessão por device. Memória e CPU ficam limitadas no `docker-compose`.

A porta nova precisa entrar no relay de QA, no firewall da VPS e no `Plano_Deployment.md` (§2.2).

### Tamanho do bloco e tempo

Medido na estação 12 em 2026-11-25 (RDWS-WE-016, `probe udp`, UDP sem DTLS na rede local): datagramas de
512 a **1400 bytes**, 20 de 20 inteiros em todos os tamanhos, com ida e volta de 131 a 172 ms. O maior
que a entrega precisa é de ~1100 (1024 + CoAP + DTLS): **bloco de 1024 bytes**.

| Enlace | RTT | Download de ~275 KB (269 blocos) |
|---|---|---|
| Wi-Fi local | ~147 ms (medido com 1024 bytes) | ~40 s, mais o tempo de apagar a flash e o do DTLS (a medir) |
| NB-IoT | 1–3 s (suposto) | ~5–15 min; o dobro com 512 |

Com blocos de 512 seriam ~90 s no Wi-Fi. Acima de 512 bytes, o driver eswifi registra um
`Cannot allocate rx packet` por datagrama: ele reserva 1460 bytes para a leitura seguinte enquanto o
pacote anterior ainda ocupa a fila, e os 16 buffers de 128 bytes não chegam. A leitura é refeita, e nada
se perde. Com 24 buffers cabe (+1.248 bytes de RAM): entra na fase 4.

## 5. A oferta (contrato)

**O3.** O device que sabe atualizar declara isso no primeiro pacote do ciclo, junto do `fw_build`:

| Nome | Valor | Significado |
|---|---|---|
| `ota` | 1 | Versão do contrato que o device entende |
| `fw_class` | texto | Classe da placa (O7) |
| `fw_rej` | texto | Build que o device recusou, só enquanto houver (O5) |
| `ota_pct` | 0–100 | Progresso, só durante um download |

Se o alvo difere do `fw_build` declarado, e não é o `fw_rej`, o `2.04` traz:

```json
{"fw": {"build": "93037114844a9152", "size": 274124, "sha256": "…", "class": "l475", "port": 5690}}
```

- **Só para quem declara `ota`.** Um firmware antigo trataria esse corpo como uma config indecodável
  (um erro de log, sem outro efeito). Nenhum dos dois lados precisa atualizar ao mesmo tempo.
- **Config primeiro.** Quando a config também está atrasada, vai só a config, e a oferta no ciclo
  seguinte. O corpo continua cabendo em um PDU (o firmware não busca Block2 da resposta).
- O custo na ingestão é uma consulta a mais por ciclo, no mesmo lugar da config. Fica atrás de um
  cache curto, e uma falha só atrasa a oferta, nunca o `2.04`.

## 6. O device (rdws_weather_node)

### Estados

```
 sem oferta ──oferta──▶ baixando ──completo──▶ verificando ──ok──▶ pendente ──reset──▶ em teste
                          │  ▲                    │                                     │
                     queda│  │retoma          SHA errado                        2.04 ─┬─┴─ revert
                          └──┘                    ▼                          confirmada │
                                              rejeitada ◀─────────────────────────────── rejeitada
```

- **Baixar:** no fim do ciclo, depois dos dados. Fecha a sessão da ingestão, abre outra com o
  `firmware_service` e pede um bloco por vez (até 1024 bytes), gravando direto no `slot1`: a imagem
  nunca fica inteira na RAM. Numa rede boa, todos os blocos saem na mesma sessão; só se divide em mais
  ciclos se algo interromper (rede, energia, edge trigger ou o limite da O10). Uma oferta igual à que
  já está em andamento continua; outra recomeça.
- **Retomar:** o cursor avança a cada setor gravado e é salvo a cada 32 KB em `settings`. Uma queda
  perde no máximo 32 KB. O Zephyr já traz isso (`STREAM_FLASH_PROGRESS`, a avaliar).
- **Verificar:** SHA-256 do `slot1` inteiro contra o da oferta, antes de pedir o teste. O MCUboot
  confere a assinatura de novo no boot.
- **Trocar:** grava em `settings` qual build está pendente, marca o teste e reinicia.

**O6.** Enquanto baixa, o device continua amostrando, e o armazenamento grava no mesmo chip. Um edge
trigger **interrompe** o download entre dois blocos e envia os dados; o download retoma no ciclo
seguinte. Num enlace lento isso é essencial: ninguém espera 15 min por um alerta de geada. O reset é
imediato depois da verificação, porque os registros já estão na flash e a perda máxima é uma amostra,
no swap (~20 s medidos).

**O10. Limite por ciclo.** `CONFIG_RDWS_OTA_MAX_S`, o tempo máximo de download por ciclo de envio
(0 = sem limite). No Wi-Fi, 0: o download dura cerca de um minuto. No NB-IoT, os 5 a 15 min estimados
seriam o rádio ligado sem pausa, o que pesa na bateria e prende o ciclo. Com um limite de 3 min, por
exemplo, 15 min viram 5 ciclos, e a atualização leva 5 envios horários. Ao atingir o limite, o device
para entre dois blocos, salva o cursor e retoma no ciclo seguinte, como numa queda de rede. Não é
rejeição. O valor do NB-IoT sai de uma medição, quando esse enlace existir.

**O5. Laço de rollback.** Sem cuidado: o alvo entra, falha no teste, reverte; o device declara o build
antigo, o servidor reoferece, e o ciclo se repete gastando rádio e flash. O que encerra isso:

1. Antes de reiniciar, o device grava `pendente = build do alvo`.
2. No boot, se roda outro build e não está em teste, o pendente **falhou**: passa a ser o rejeitado.
3. O rejeitado vai em `fw_rej`, e o device **não baixa** de novo esse build.
4. O servidor não o oferece e mostra `rejected`. Mudar o alvo para outro build volta a oferecer.

O mesmo vale para um SHA-256 errado. Uma queda no download **não** é rejeição.

### Orçamento

- RAM: o build com MCUboot usa 67,4 KB de 96 KB. O buffer de bloco cabe no `request_buf`/`response_buf`
  já existentes. O contexto de escrita e o hash entram na medição da fase 4.
- Watchdog: o download alimenta o canal do `uplink` a cada bloco. Apagar um setor não passa de 1 s.
- Flash: `STREAM_FLASH_ERASE` apaga por setor, sob demanda, só dentro do `slot1`.

### Falhas previstas

| Situação | Efeito |
|---|---|
| Rede cai no meio | Retoma do cursor no ciclo seguinte |
| Energia cai no meio | Idem, do último cursor salvo; o slot0 não é tocado |
| Limite de tempo do ciclo (O10) | Retoma do cursor no ciclo seguinte |
| Servidor sem a imagem (`4.04`) | Desiste do ciclo; tenta no próximo |
| SHA-256 errado | Rejeitada |
| Assinatura errada | MCUboot recusa; o device volta na mesma imagem e a marca rejeitada |
| Imagem trava ou não envia | Rollback (RDWS-WE-012); rejeitada |
| Queda de energia no swap | MCUboot termina ao religar (validado) |
| Config nova recusada pela imagem nova | `cfg_rej`, já existente; não é rollback |

## 7. Placa errada

**O7.** A assinatura garante a origem, não a placa. Uma imagem da L4S5 assinada com a mesma chave
passaria pela validação e, no melhor caso, travaria e reverteria.

- **No device:** compara a classe da oferta (`class`) com a sua antes de baixar.
- **No servidor:** recusa um alvo cuja `board_class` difere da declarada em `fw_class`.
- **No MCUboot:** `MCUBOOT_UUID_CID` confere a classe gravada na imagem (`imgtool --cid`). É a única
  defesa que não depende de os outros dois estarem certos, e só entra com o bootloader regravado por
  cabo. A estação 12 ainda está ao alcance. O port do Zephyr declara a conferência mas não a implementa
  (o build não liga, visto em 2026-11-25): as funções vêm de um módulo nosso, compilado só no MCUboot.
  Uma imagem sem a classe passa a ser recusada, **inclusive a que já está no slot0**, então bootloader e
  aplicação são regravados juntos.

## 8. Segurança

- A chave de assinatura nunca vai para o servidor nem para o repositório. A de desenvolvimento assina
  só placas de bancada; a de produção fica em secret de CI.
- Servidor comprometido: o atacante pode **negar** atualizações, ou oferecer uma imagem **antiga**
  legítima. Não consegue instalar uma que a chave não assinou. O rebaixamento só é barrado com
  `MCUBOOT_DOWNGRADE_PREVENTION`, que usa a versão da imagem. Hoje ela é sempre 0.1.0, então só
  serve depois do pipeline de release.
- `GET /fw` só entrega o alvo do device autenticado: um device não lista nem baixa outras imagens.
- O volume de imagens é só leitura para o `firmware_service`.

## 9. Fases

Cada uma com entrega testada e commit próprio, no padrão do projeto. As de servidor não tocam na
estação; as de firmware são testadas contra um servidor de bancada antes do QA.

| Fase | Onde | Entrega | Pronto quando |
|---|---|---|---|
| **0** ✅ | firmware | Datagrama de ~1100 bytes pelo eswifi; classe da placa no MCUboot (O7) | Feita em 2026-11-25 (RDWS-WE-016): 1400 bytes inteiros; imagem de outra classe, ou sem classe, recusada |
| **1** | rdws_us | Tabelas, registro, alvo, REST, estado derivado | Testes; um alvo definido aparece em `firmware-status` |
| **2** | rdws_us | `firmware_service` (GET com Block2, PSK, ETag) | Baixa a imagem inteira com um cliente CoAP e o SHA-256 confere; duas estações ao mesmo tempo; ingestão sem atraso medido |
| **3** | rdws_us | Oferta no `2.04` | Só a quem declara `ota`; config tem prioridade; `fw_rej` suprime |
| **4** | firmware | Download, retomada, verificação (sem reiniciar) | Testes `native_sim`; queda no meio e retomada; SHA errado recusado; RAM e flash medidos |
| **5** | firmware | Pedido de teste automático, memória de rejeição, `ota_pct` | Laço de rollback encerrado na bancada |
| **6** | os dois | Ponta a ponta na estação 12 | Tabela da §6 exercitada; medição de tempo |
| **7** | rdws_us | Painel: versão, alvo, estado e rejeições por device | Conferido no Grafana local e no QA |

Fases 1 a 3 e 0, 4 podem andar em paralelo. A 5 depende da 4, e a 6 depende das demais.

### Fora deste plano

Atualização diferencial (fase posterior, pensada para NB-IoT), grupos e rollout gradual, CI de release
com chave de produção, proteção contra rebaixamento, atualização do próprio MCUboot.

## 10. Riscos

| Risco | Probabilidade | O que reduz |
|---|---|---|
| Eswifi não entrega 1100 bytes com DTLS | Baixa | Medido sem DTLS até 1400 (fase 0); se falhar com DTLS, 512, pouco mais do dobro do tempo |
| Download longo bloqueia o ciclo ou gasta bateria | Alta em NB-IoT | O6: cede entre blocos ao edge trigger; O10: limite por ciclo |
| Laço de rollback | Certa sem O5 | O5, testada na fase 5 |
| Escrita fora do `slot1` apaga registros | Baixa, grave | Só a API de partição do Zephyr; teste na fase 4 |
| Perder a chave de produção | Baixa, grave | Backup; o MCUboot aceita mais de uma chave pública, o que permite trocar com antecedência |
| Porta nova exposta | Baixa | PSK por device, limite por IP, relay sem conhecer a PSK |
