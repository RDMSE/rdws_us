Data: 2026-11-25

# Plano de OTA (atualização de firmware pela rede)

Como uma estação recebe, valida e adota uma imagem nova sem cabo, e como o servidor sabe quem roda o
quê. Cobre os dois repositórios: `rdws_us` (registro, entrega, acompanhamento) e
`rdws_weather_node` (download, troca, rollback).

**Estado:** decisões aceitas em 2026-11-25, revistas no mesmo dia com uma revisão externa (O11 a O20);
fases 0 (RDWS-WE-016) e 1 (RDWS-241, RDWS-WE-018) feitas. Próxima: fase 2.

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
| O11 | O que confirma a imagem, e o que uma volta significa? | Só um `2.04` confirma; 24 h de prazo; a volta só rejeita com prova de falha, ou na segunda vez | 6 |
| O12 | Proteção contra downgrade? | Contador de segurança no MCUboot; **adiada**, obrigatória antes da produção | 8 |
| O13 | O que a retomada garante? | Ponto salvo só em limite de setor, depois de gravar e conferir | 6 |
| O14 | Como se impede gravar além do `slot1`? | Tamanho conferido no servidor e no device, contra o `slot1` menos o trailer | 6 |
| O15 | Como se protege uma estação da variante de bancada? | O servidor recusa `bench` sem `allowBench`, e o histórico registra | 3 |
| O16 | Quem arbitra a QSPI? | O driver serializa; o OTA nunca segura mais que um setor de 4 KB | 6 |
| O17 | Como não atualizar a frota de uma vez? | Device nasce sem permissão (`ota_enabled`); ondas na fase 8; limite de downloads simultâneos | 3, 4 |
| O18 | Como se desfaz uma rejeição? | Nova tentativa pedida pelo admin, com um número que o device compara | 5, 6 |
| O19 | Como a publicação evita arquivo pela metade? | Copia com nome temporário e renomeia antes de registrar | 3 |
| O20 | E se o alvo muda ou é retirado no meio do download? | Outra oferta descarta a parcial; `4.04` larga o ciclo e a parcial fica | 6 |

**Têm prazo, porque o MCUboot não se atualiza em campo:** a O7, feita na fase 0, e a **O12**, que precisa
entrar antes de a primeira estação ficar sem cabo.

## 1. O que já existe

| Peça | Situação | Onde |
|---|---|---|
| MCUboot: swap, confirmação, rollback, assinatura ECDSA-P256 | Validado na estação 12 (2026-11-25) | RDWS-WE-012 |
| Imagem em teste até o primeiro `2.04` de leituras; 3 h de limite (passa a 24 h na fase 5, O11) | Validado com 300 s | RDWS-WE-012 |
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
  id, public_id, build_id (16 hex, único), sha256 (único), size_bytes, board_class, version,
  variant ('bench' | 'field'), notes, created_at, created_by, withdrawn_at, withdrawn_by

device_firmware                              1:1 com o device, criada com ele (trigger)
  device_id (PK), device_public_id, ota_enabled (padrão false), ota_changed_at, ota_changed_by

device_firmware_targets                      uma linha por alvo definido; a atual é a sem replaced_at
  id, device_id, image_id, attempt, allow_bench, set_at, set_by, replaced_at, replaced_by
```

Um alvo retirado (`withdrawn_at`) deixa de ser oferecido, e as imagens já instaladas não mudam. Os alvos
antigos ficam: uma imagem ruim que chegou a uma estação precisa ser rastreável.

**O4, O17. Nunca a frota toda.** O alvo é por device e manual, e não há chamada que atinja vários. Em
duas camadas:

1. **Fase 1.** Todo device nasce com `ota_enabled = false`. Sem permissão ele não recebe oferta, mesmo com
   alvo. O admin habilita um a um.
2. **Fase 8.** Uma imagem, uma lista ordenada de **ondas**; a primeira é o canário. A seguinte só sai por
   ação do admin, depois de a anterior ficar `applied` por um tempo, e **para sozinha** se algum device
   rejeitar ou reverter. Ela usa as mesmas tabelas de alvo.

**O15. Variante.** A de bancada (`bench`) traz comandos destrutivos (`rdws fault`, `img erase`). Não se
confia na atenção de quem escolhe: o servidor recusa definir um alvo `bench` sem `allowBench: true`, e a
linha do alvo guarda quem autorizou.

**O8.** A imagem é assinada na máquina que a compila. O servidor recebe um arquivo pronto e nunca a
chave. `scripts/fw_publish.py` (no repositório do firmware):

1. confere a assinatura com a chave pública;
2. lê build ID, versão, tamanho e classe da placa do próprio artefato;
3. copia para o volume com um nome temporário, renomeia para `<sha256>.bin` (**O19**) e chama a rota de
   registro. A cópia é por um container avulso com o volume montado, porque o `firmware_service` o monta
   só para leitura.

O servidor **não confia no script**: lê do próprio arquivo o SHA-256, o tamanho, a classe e o build ID
(a nota ELF na imagem), e recusa o que não for uma imagem do MCUboot íntegra. Conferido em 2026-11-25 nas
imagens da estação 12: o build ID lido do arquivo é o que a placa declara. Recusa também uma imagem
maior que a do `slot1` (O14). O upload não passa pelo gateway, porque ~274 KB em JSON pelo broker não
fazem sentido.

**O9.** A estação 12 recebe a variante de bancada até haver release.

### REST (admin)

| Rota | Efeito |
|---|---|
| `POST /firmware-images` | Registra uma imagem já copiada |
| `GET /firmware-images` | Lista |
| `POST /firmware-images/{id}/withdraw` | Deixa de oferecer |
| `PUT /devices/{id}/firmware-target` | Define o alvo (`allowBench` para bancada); `null` remove |
| `POST /devices/{id}/firmware-target/retry` | Nova tentativa depois de uma rejeição (O18) |
| `PUT /devices/{id}/firmware-ota` | Habilita ou desabilita (`ota_enabled`) |
| `GET /devices/{id}/firmware-status` | Imagem declarada, alvo, permissão e estado |

Estado derivado do que a estação declara, nunca gravado à mão: `none` (sem alvo), `disabled` (com alvo,
sem permissão), `pending` (alvo diferente do declarado), `downloading`, `rejected` (o device declarou
rejeição deste alvo), `applied` (declara o build do alvo).

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
- No máximo N downloads simultâneos (configurável); os demais recebem `5.03` com `Max-Age`, e tentam no
  ciclo seguinte. Protege o servidor e a ingestão de uma onda inteira acordando junto (O17).

A porta é a **5690/udp**. O relay de QA (5691), o firewall da VPS e o compose de prod estão descritos no
`Plano_Deployment.md` (§2.2), e só entram quando uma estação fora da LAN precisar.

Como ficou (RDWS-244):

- O Block2 é tratado pelo serviço, sem estado: cada bloco é lido do arquivo. O modo de blocos da libcoap
  guardaria a imagem por sessão e responderia os seguintes sem perguntar, e um alvo trocado não mudaria
  o `ETag`. Retomar em qualquer bloco, em outra sessão, sai de graça.
- Baixa quem tem alvo atual, `ota_enabled` e imagem não retirada; qualquer outro caso é `4.04`, sem dizer
  qual. O alvo vale 5 s em cache: uma retirada ou um alvo novo valem no meio do download.
- 4 downloads simultâneos (`FIRMWARE_MAX_DOWNLOADS`). A vaga é de quem pediu um bloco nos últimos 30 s, e
  acaba no último bloco, que não ocupa vaga: a resposta dele pode se perder, e o device o pede de novo.
  Além do limite, `5.03` com `Max-Age: 3600`. Banco fora do ar: `5.03` com `Max-Age: 30`.
- `5.00` se o arquivo falta ou não tem o tamanho registrado. O `Size2` vai no bloco 0.
- A credencial é conferida a cada pedido: uma revogada perde o acesso numa sessão aberta.

### Tamanho do bloco e tempo

Medido na estação 12 em 2026-11-25 (RDWS-WE-016, `probe udp`, UDP sem DTLS na rede local): datagramas de
512 a **1400 bytes**, 20 de 20 inteiros em todos os tamanhos, com ida e volta de 131 a 172 ms. O maior
que a entrega precisa é de ~1100 (1024 + CoAP + DTLS): **bloco inicial de 1024 bytes**. O servidor aceita
menores, então nada fica congelado antes da medida com CoAP e DTLS, que é o primeiro teste da fase 4.

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
| `fw_rej` | texto | `<build>/<tentativa>/<causa>` do que o device recusou, só enquanto houver (O5, O11, O18) |
| `ota_pct` | 0–100 | Progresso, só durante um download |

Se o alvo difere do `fw_build` declarado, e não é o `fw_rej`, o `2.04` traz:

```json
{"fw": {"build": "93037114844a9152", "size": 274124, "sha256": "…", "class": "ef76f59e-…", "port": 5690, "try": 1}}
```

`class` é o UUID da placa (O7). `try` é o número da tentativa (O18): sobe quando o admin pede uma nova, e
o device ignora a própria rejeição se o da oferta for maior que o da rejeição guardada.

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
- **Retomar:** o ponto é salvo a cada 32 KB em `settings`. Uma queda perde no máximo 32 KB. Contrato na
  O13.
- **Verificar:** SHA-256 do `slot1` inteiro contra o da oferta, antes de pedir o teste. O MCUboot
  confere a assinatura e a classe de novo no boot.
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

**O13. Contrato da retomada.**

- O ponto salvo é sempre um **limite de setor** (4 KB), e cobre só bytes já gravados e relidos.
- Ordem: apaga o setor, grava, relê e confere, e **só então** salva o ponto.
- Na retomada, tudo a partir do ponto é apagado de novo antes de ser gravado.
- O `stream_flash` do Zephyr serve (`STREAM_FLASH_PROGRESS`), com um cuidado: ao carregar o progresso ele
  considera apagado o resto do setor do **último byte** salvo. Com o ponto no meio de um setor, os
  bytes gravados depois dele não seriam apagados, e a regravação corromperia. Em limite de setor, o
  seguinte é apagado (lido no código em 2026-11-25; testado na fase 4).
- Testes com queda **durante o apagar, durante o gravar, e logo antes e logo depois de salvar o ponto**.

**O14. Tamanho antes de começar.** O device recusa uma oferta com `size` zero ou maior que o `slot1` **menos
o trailer do MCUboot**, e calcula `offset + tamanho` sem estouro de 32 bits. O limite vem da partição do
devicetree, nunca da oferta. A escrita usa só a API de partição, que também recusa, e o limite do
`slot1` é o último antes do LittleFS.

**O16. Acesso à QSPI.** O driver do Zephyr serializa cada operação com uma trava, então escrita no `slot1`,
LittleFS e leitura para envio não se misturam. O que resta é **não segurar a trava por muito tempo**:

- o OTA apaga um setor de 4 KB por vez, nunca blocos maiores (`img erase`, da bancada, é a exceção);
- o `slot1` e o LittleFS são partições separadas, então uma não corrompe a outra;
- a ordem é: o envio dos dados e as gravações do `storage` têm prioridade; o download acontece no fim do
  ciclo e cede entre blocos (O6).

**O20. Alvo que muda.** Uma oferta de outro build, tamanho ou SHA-256 descarta a transferência anterior: o
ponto salvo é apagado antes de gravar o primeiro bloco novo. Se a imagem é retirada, o servidor responde
`4.04`: o device larga o ciclo e a parcial fica no `slot1`, sem efeito, até a próxima oferta.

**O11. Confirmação e o que a volta significa.** Só um `2.04` de um pacote com amostra da própria imagem
confirma (RDWS-WE-012). Confirmar só localmente foi considerado e **descartado**: uma imagem que quebre o
Wi-Fi ou o DTLS está perfeita por dentro e, confirmada, não pode mais ser consertada pela rede. É a única
falha que o OTA não pode deixar passar.

O custo é o caso inverso: uma imagem boa numa rede fora do ar. Por isso:

- o prazo passa de 3 h para **24 h**. Enquanto espera, a estação amostra e guarda normalmente;
- a volta só **rejeita** quando há prova de falha da imagem. Sem prova, é inconclusiva, e vale uma segunda
  tentativa:

| O que houve | Como o device sabe | Efeito |
|---|---|---|
| Crash ou travamento em teste | Marca deixada no registrador de backup pelo tratador de falha; `reset_reason` de watchdog | Rejeitada |
| Classe ou assinatura recusada pelo MCUboot | Nunca rodou: a marca "rodou" não foi gravada | Rejeitada |
| SHA-256 errado | Conferência antes do teste | Rejeitada |
| Prazo sem `2.04` | Gravado antes do reset | Inconclusiva |
| Queda de energia ou reset em teste | Nenhuma das acima | Inconclusiva |

Duas voltas inconclusivas do mesmo build rejeitam. A contagem é por build, zera ao confirmar e fica em
`settings`. A marca fica no registrador reservado ao boot (`BKP_REG_BOOT`), porque o registro de falha em
RAM pode estar em outro endereço na imagem antiga. Numa queda de energia o registrador se perde, o que
cai na linha certa: inconclusiva.

**O5. Laço de rollback.** Sem cuidado: o alvo entra, falha no teste, reverte; o device declara o build
antigo, o servidor reoferece, e o ciclo se repete gastando rádio e flash. O que encerra isso:

1. Antes de reiniciar, o device grava `pendente = build do alvo`.
2. No boot, se roda outro build e não está em teste, o pendente voltou, e a O11 decide: rejeitado, ou
   inconclusivo (nova tentativa).
3. O rejeitado vai em `fw_rej`, e o device **não baixa** de novo esse build.
4. O servidor não o oferece e mostra `rejected`. Mudar o alvo para outro build volta a oferecer, e uma
   **nova tentativa do mesmo build** é uma ação do admin (O18).

Uma queda no download **não** é rejeição.

### Orçamento

- RAM: o build com MCUboot usa 67,4 KB de 96 KB. O buffer de bloco cabe no `request_buf`/`response_buf`
  já existentes. O contexto de escrita e o hash entram na medição da fase 4.
- Watchdog: o download alimenta o canal do `uplink` a cada bloco. Apagar um setor não passa de 1 s.
- Flash: `STREAM_FLASH_ERASE` apaga por setor, sob demanda, só dentro do `slot1`. O `settings` fica em
  outra partição, na flash interna.

### Falhas previstas

| Situação | Efeito |
|---|---|
| Rede cai no meio | Retoma do cursor no ciclo seguinte |
| Energia cai no meio | Idem, do último cursor salvo; o slot0 não é tocado |
| Limite de tempo do ciclo (O10) | Retoma do cursor no ciclo seguinte |
| Servidor sem a imagem (`4.04`) | Desiste do ciclo; tenta no próximo |
| Servidor cheio (`5.03`) | Respeita `Max-Age`; tenta no ciclo seguinte |
| Tamanho maior que o permitido (O14) | Oferta recusada, sem baixar |
| SHA-256 errado | Rejeitada |
| Assinatura ou classe errada | MCUboot recusa; o device volta na mesma imagem e a marca rejeitada |
| Imagem trava | Rollback (RDWS-WE-012); rejeitada |
| Imagem sem rede, ou queda de energia em teste | Rollback; inconclusiva, uma segunda tentativa (O11) |
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
  legítima. Não consegue instalar uma que a chave não assinou.
- **O12, downgrade, adiado.** A proteção é do MCUboot, que funciona com o nosso modo de swap (o código só a
  aplica ao swap de teste, então a reversão não é afetada). Há duas formas: por **versão**, que impediria
  voltar de propósito a uma versão anterior, e por **contador de segurança**, que só sobe quando importa.
  A escolhida é a segunda. Adiar não atrapalha as fases 1 a 6, mas cobra depois: ligá-la pede regravar o
  bootloader **por cabo** em cada estação, e as que já estiverem sem cabo não a terão. **Obrigatória antes
  de a primeira estação ficar fora do alcance de um cabo, e antes de o pipeline de produção ser
  considerado pronto.** A imagem de slot0 sem contador aceita qualquer uma, então ligar depois é
  compatível com as imagens já instaladas.
- Três propriedades distintas, com mecanismos distintos: **autenticidade** (ECDSA-P256, MCUboot),
  **integridade** (SHA-256) e **autorização da entrega** (PSK por device). Nenhuma substitui a outra.
- `GET /fw` só entrega o alvo do device autenticado: um device não lista nem baixa outras imagens.
- O volume de imagens é só leitura para o `firmware_service`.

## 9. Fases

Cada uma com entrega testada e commit próprio, no padrão do projeto. As de servidor não tocam na
estação; as de firmware são testadas contra um servidor de bancada antes do QA.

| Fase | Onde | Entrega | Pronto quando |
|---|---|---|---|
| **0** ✅ | firmware | Datagrama de ~1100 bytes pelo eswifi; classe da placa no MCUboot (O7) | Feita em 2026-11-25 (RDWS-WE-016): 1400 bytes inteiros; imagem de outra classe, ou sem classe, recusada |
| **1** ✅ | rdws_us e firmware (`fw_publish.py`) | Tabelas, registro, alvo, permissão, REST, estado derivado | Feita em 2026-11-25 (RDWS-241, RDWS-WE-018): no QA, duas imagens publicadas e, na estação 12, `none` → `applied` → `disabled` → `pending` → `none`; `bench` sem `allowBench` recusado |
| **2** | rdws_us | `firmware_service` (GET com Block2, PSK, ETag) | Baixa a imagem inteira com um cliente CoAP e o SHA-256 confere; duas estações ao mesmo tempo; ingestão sem atraso medido |
| **3** | rdws_us | Oferta no `2.04` | Só a quem declara `ota`; config tem prioridade; `fw_rej` suprime |
| **4** | firmware | Download, retomada, verificação (sem reiniciar) | **Primeiro:** bloco de 1024 com CoAP e DTLS. Testes `native_sim`; queda durante o apagar, o gravar, e antes e depois de salvar o ponto (O13); `size` acima do limite, zero e no estouro recusados, sem tocar em nada além do `slot1` (O14); SHA errado recusado; registros em LittleFS intactos depois do download; RAM e flash medidos |
| **5** | firmware | Pedido de teste automático, causas da volta (O11), nova tentativa (O18), prazo de 24 h, `ota_pct` | Laço de rollback encerrado na bancada; cada linha da tabela da O11 provocada |
| **6** | os dois | Ponta a ponta na estação 12 | Tabela da §6 exercitada; medição de tempo |
| **7** | rdws_us | Painel: versão, alvo, estado e rejeições por device | Conferido no Grafana local e no QA |
| **8** | rdws_us | Ondas de rollout (O17) | Canário primeiro; a seguinte só por ação do admin; para sozinha numa rejeição |

Fases 1 a 3 e 0, 4 podem andar em paralelo. A 5 depende da 4, a 6 depende das demais, e a 8 não bloqueia
nenhuma: com poucas estações, a permissão por device basta.

### Fora deste plano

Atualização diferencial (fase posterior, pensada para NB-IoT), CI de release com chave de produção,
atualização do próprio MCUboot. A proteção contra downgrade está decidida e adiada (O12).

## 10. Riscos

| Risco | Probabilidade | O que reduz |
|---|---|---|
| Eswifi não entrega 1100 bytes com DTLS | Baixa | Medido sem DTLS até 1400 (fase 0); se falhar com DTLS, 512, pouco mais do dobro do tempo |
| Download longo bloqueia o ciclo ou gasta bateria | Alta em NB-IoT | O6: cede entre blocos ao edge trigger; O10: limite por ciclo |
| Laço de rollback | Certa sem O5 | O5, testada na fase 5 |
| Imagem boa rejeitada por falta de rede | Média | O11: volta inconclusiva tem segunda tentativa, prazo de 24 h, e nova tentativa do admin (O18) |
| Imagem que quebra a rede é confirmada | Baixa, sem conserto remoto | O11: só um `2.04` confirma |
| Retomada no meio de um setor corrompe a imagem | Alta sem O13 | O13: ponto em limite de setor; e o SHA-256 antes do teste |
| Escrita fora do `slot1` apaga registros | Baixa, grave | O14 e só a API de partição do Zephyr; teste na fase 4 |
| Frota inteira atualizada com uma imagem ruim | Baixa | O17: permissão por device, ondas, downloads limitados |
| Downgrade para uma imagem antiga legítima | Baixa até a produção | O12, obrigatória antes dela |
| Perder a chave de produção | Baixa, grave | Backup; o MCUboot aceita mais de uma chave pública, o que permite trocar com antecedência |
| Porta nova exposta | Baixa | PSK por device, limite por IP, relay sem conhecer a PSK |
