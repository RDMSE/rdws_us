Data: 2026-07-12

# Plano de Provisionamento de Credenciais DTLS por Device

Fecha o ponto em aberto registrado tanto no `Plano_Ingestion.md` quanto no
`Plano_SensorSimulatorService.md`: como cada device (real ou simulado) obtém e usa
credenciais PSK para o handshake DTLS com o `IngestionService`.

---

## Visão geral do fluxo

```
device.create --> device_credential.provision --> (chave em texto puro, uma única vez)
                                |
                                v
                    device_credentials (chave criptografada em repouso)
                                |
                                v
IngestionService/SensorSimulatorService --startup--> carrega cache em memória
                                |
                    device_credential.changed (EventBus) --> invalida cache pontual
```

---

## 1. Armazenamento

Tabela nova, separada de `device_configurations`:

```sql
CREATE TABLE device_credentials (
    id              BIGINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
    device_id       BIGINT NOT NULL UNIQUE REFERENCES devices(id),
    psk_identity    UUID NOT NULL DEFAULT gen_random_uuid() UNIQUE,
    psk_key_enc     BYTEA NOT NULL,          -- AES-256-GCM: nonce || ciphertext || tag
    status          VARCHAR(16) NOT NULL DEFAULT 'active', -- active | revoked
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    rotated_at      TIMESTAMPTZ,
    revoked_at      TIMESTAMPTZ
);
```

**Por que não em `device_configurations.config` (JSONB):** `device_config.get`
(`Plano_API_REST.md`) é uma capability de leitura geral, sem controle de campo —
misturar segredo de autenticação com config operacional (faixas de sensor, período,
etc.) arrisca vazar a chave PSK pelo mesmo endpoint que devolve config comum. Tabela
própria, sem capability HTTP pública associada, é a fronteira de segurança mais simples
de garantir.

## 2. Identidade PSK

`psk_identity` (UUID) é o hint usado no handshake DTLS para localizar a chave — **não
usa `devices.id`** (BIGSERIAL), para não reproduzir na camada de rede o mesmo problema
de enumeração já identificado na Fase 13 do `Plano_Gateway_HTTP.md` (IDOR).

A Fase 13 (backlog, sem data) prevê `public_id` para a API REST; como este plano não
deve ficar bloqueado por ela, `psk_identity` é gerado de forma independente agora.
Convergir os dois no futuro é possível, mas não obrigatório — propósitos diferentes
(um é identificador de API REST, outro é identidade de protocolo DTLS).

## 3. Chave em repouso

Diferente de senha de usuário (`bcrypt`/`argon2`, comparação por hash), o PSK precisa
ser **recuperado em texto puro** pelo `IngestionService` para o handshake DTLS — hash
unidirecional não serve aqui.

- Criptografia simétrica **AES-256-GCM** com chave mestra de aplicação
  (`CREDENTIAL_ENCRYPTION_KEY`), mesmo padrão já usado para `JWT_SECRET`: variável de
  ambiente injetada via `environment:`/GitHub Secrets, nunca hardcoded na imagem, um
  valor por ambiente (dev/qa/prod).
- `psk_key_enc` guarda `nonce || ciphertext || tag` concatenados; decrypt acontece só em
  memória, no momento do uso.

## 4. Provisionamento

| Capability | Momento | Comportamento |
|---|---|---|
| `device_credential.provision` | Logo após `device.create` (ou no mesmo handler, atomicamente) | Gera 32 bytes aleatórios (CSPRNG), criptografa e persiste; retorna a chave **em texto puro uma única vez** na resposta — mesmo padrão de API key exibida só na criação |

Após esse retorno, a chave nunca mais é exposta em texto puro por nenhum endpoint HTTP.

## 5. Consumo pelo IngestionService e pelo SensorSimulatorService

Mantendo o princípio já registrado no `Plano_Ingestion.md` de que o `IngestionService`
é stateless em relação à persistência:

- Nova capability **interna**, sem rota HTTP pública associada no `EventRouter`:
  `device_credential.get_active` — acessível só pelo protocolo socket que os demais
  microserviços já usam para falar com o gateway (mesmo canal de `PersistenceService`/
  `AuthService`).
- **Startup**: `IngestionService` conecta no gateway (mesmo padrão dos outros serviços) e
  carrega todas as credenciais ativas para uma tabela em memória
  (`psk_identity → chave decriptada`).
- **Refresh**: publica/assina `device_credential.changed` no EventBus interno (mesmo
  mecanismo de `request.completed`/`metrics.snapshot`) — ao revogar/rotacionar um device,
  o `IngestionService` invalida a entrada correspondente sem restart nem polling.
- **`SensorSimulatorService`** usa um caminho mais simples que o do `IngestionService`,
  por ser cliente (não servidor): já abre uma nova conexão por device a cada ciclo
  (passo 3 do fluxo, `Plano_SensorSimulatorService.md`) — em vez de manter cache e
  assinar `device_credential.changed`, basta chamar `device_credential.get_active`
  **imediatamente antes de abrir cada conexão DTLS**, a cada ciclo de envio. Isso evita
  replicar o mecanismo de cache/invalidação do lado servidor (desnecessário para um
  cliente que já reconecta por ciclo) e garante que uma rotação nunca deixa o simulador
  usando credencial revogada até o próximo restart — o mesmo comportamento que o
  hardware real vai precisar ter.

**Nota sobre segurança do canal interno:** o protocolo socket broker↔serviços
(`tcp://`/`unix://`, `Plano_Deployment.md` §2) não tem TLS — é framing próprio sobre
TCP/UNIX socket puro. Isso significa que a credencial PSK decriptada trafega em texto
puro nesse canal (do gateway até o `IngestionService`/`SensorSimulatorService`), dentro
da rede interna do Docker Compose. **Decisão explícita por ora:** aceitável, porque essa
rede já não é exposta externamente (mesmo modelo de confiança já assumido para
`JWT_SECRET` e para o payload de todas as outras capabilities internas) — mas é uma
superfície diferente de "config normal", então fica registrado aqui em vez de implícito.
Se isso vier a incomodar (ex. ao migrar para múltiplos hosts/k8s, onde a rede interna
deixa de ser um único host de confiança), a mitigação natural é TLS no canal
broker↔serviços (mTLS, reaproveitando a mesma infra de certificados que o DTLS já usa
do lado dos devices) — não implementado agora, registrado como ponto em aberto abaixo.

## 6. Rotação e revogação

- `device_credential.rotate` — gera nova chave, marca a antiga `revoked`, sem janela de
  sobreposição por enquanto (hardware ainda não está em campo, então não há o problema
  clássico de device com chave antiga já gravada na flash — grace period fica para
  quando isso virar um problema real).
- `device_credential.revoke` — marca `status = revoked`, `revoked_at = now()`;
  `IngestionService` rejeita handshake na próxima tentativa após o evento de invalidação
  chegar.

## 7. Rotação da chave mestra (KEK)

Não coberta nas seções anteriores: se `CREDENTIAL_ENCRYPTION_KEY` precisar ser trocada
(ex. comprometimento suspeito), toda a tabela `device_credentials` fica ilegível com a
chave nova, e re-criptografar em massa exige coexistência temporária de chaves — sem
isso, é impossível trocar a KEK sem downtime ou sem um processo de migração dedicado.

**Desenho mínimo, para quando isso virar necessário:**
- Coluna `key_version SMALLINT NOT NULL DEFAULT 1` em `device_credentials`, indicando
  qual versão da KEK criptografou aquela linha.
- Múltiplas chaves mestras podem coexistir via env vars versionadas
  (`CREDENTIAL_ENCRYPTION_KEY_V1`, `CREDENTIAL_ENCRYPTION_KEY_V2`, ...) — decrypt escolhe
  a chave pelo `key_version` da linha; encrypt de linhas novas sempre usa a versão mais
  recente.
- Job de re-criptografia em batch (fora do caminho crítico de request) migra linhas
  antigas para a versão nova, atualizando `key_version` conforme processa.

Adiado por ora — sem necessidade real ainda — mas registrado aqui em vez de ficar de
fora do documento, no mesmo espírito do grace period da seção 6.

## 8. Biblioteca CoAP/DTLS (lado servidor, C++)

**libcoap** — madura, integra com TinyDTLS ou OpenSSL/mbedTLS como backend DTLS, com
suporte nativo a callback de PSK por identidade (`coap_context_set_psk2`/
`coap_dtls_spsk_info_t`) — encaixa diretamente no formato do cache em memória descrito
na seção 5.

---

## Pontos em aberto

- Nenhum bloqueante para o `IngestionService`/`SensorSimulatorService` avançarem — os
  pontos abaixo podem ser resolvidos em paralelo:
- Definir se `device_credential.provision` roda automaticamente dentro do handler de
  `device.create` ou como chamada separada explícita pelo operador.
- Avaliar, quando a Fase 13 (`public_id`) do `Plano_Gateway_HTTP.md` sair do backlog, se
  faz sentido convergir `psk_identity` com `public_id` ou manter os dois separados.
- Grace period na rotação, quando o hardware físico entrar em produção (chave antiga
  ainda válida por N minutos/horas após a nova ser gerada).
- Versionamento da KEK (seção 7) — adiado até virar necessidade real (comprometimento
  suspeito, política de rotação periódica, etc.).
- TLS/mTLS no canal broker↔serviços (seção 5) — hoje aceito como confiável por estar
  restrito à rede interna do Docker Compose; reavaliar se a topologia migrar para
  múltiplos hosts/k8s.
