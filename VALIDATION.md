# VALIDATION.md — Token Quota Display

Resultado da Fase 1 (validação obrigatória) executada em 2026-08-28 na máquina local (macOS), antes de qualquer código do coletor ou do firmware.

## 1. Caminhos reais encontrados

### Claude Code
- Raiz: `~/.claude/`
- Logs de conversa/uso: `~/.claude/projects/<projeto-slug>/<session-uuid>.jsonl`
  - Um diretório por projeto (slug = caminho absoluto do projeto com `/` trocado por `-`).
  - Um arquivo `.jsonl` por sessão dentro do diretório do projeto.
  - Exemplo real: `~/.claude/projects/-Users-henricksantana-...-cubo/3d4d9966-7418-4630-8505-1779c4ae75f1.jsonl`
- Contagem no momento da validação: **11 arquivos `.jsonl`** espalhados em 2 projetos (`cubo` e `server-altlabs`).
- Não há um arquivo único "global" de uso — o coletor precisa varrer `~/.claude/projects/**/*.jsonl` (glob recursivo).

### Codex CLI
- Raiz: `~/.codex/`
- Logs de sessão: `~/.codex/sessions/AAAA/MM/DD/rollout-<timestamp>-<uuid>.jsonl`
  - Particionado por ano/mês/dia.
  - Exemplo real: `~/.codex/sessions/2026/08/26/rollout-2026-08-26T21-38-31-01a040a7-74ab-7fe0-998a-7f7420d7b382.jsonl`
- Contagem no momento da validação: **7 arquivos `.jsonl`**, distribuídos em subpastas de dias distintos de agosto/2026.
- O coletor precisa varrer `~/.codex/sessions/**/*.jsonl` (glob recursivo, 3 níveis de data).

## 2. Esquema de chaves observado (amostra de 3 linhas por fonte)

Nenhum conteúdo de prompt foi lido ou impresso — apenas nomes de chaves e valores numéricos/estruturais.

### Claude Code — linhas de evento (nível superior)
O JSONL do Claude Code mistura vários tipos de evento na mesma sessão. Nas 3 primeiras linhas do arquivo mais recente:
```
('bridgeSessionId', 'lastSequenceNum', 'ownerAccountUuid', 'ownerOrganizationUuid', 'sessionId', 'type')
('operation', 'sessionId', 'timestamp', 'type')
('operation', 'sessionId', 'timestamp', 'type')
```
Variando o arquivo, aparecem outros tipos de linha, entre eles o que interessa ao coletor — mensagens de assistant com uso de tokens:
```
('cwd', 'effort', 'entrypoint', 'gitBranch', 'isSidechain', 'message', 'parentUuid',
 'requestId', 'sessionId', 'timestamp', 'type', 'userType', 'uuid', 'version')
```
Dentro de `message.usage` (chaves observadas, idênticas nos dois projetos amostrados):
```
input_tokens, cache_creation_input_tokens, cache_read_input_tokens, output_tokens,
output_tokens_details{thinking_tokens}, server_tool_use{...}, service_tier,
cache_creation{ephemeral_1h_input_tokens, ephemeral_5m_input_tokens}, inference_geo,
iterations[...], speed
```
- Campo `timestamp` no nível da linha (irmão de `message`), formato ISO 8601 UTC com `Z`: `"2026-08-15T16:45:35.636Z"`.
- **Não existe** campo de percentual de cota explícito (ex.: `session_pct`, `usage_percent`) em nenhuma linha observada.

### Codex CLI — linhas de evento (nível superior)
Todas as linhas amostradas têm o mesmo formato:
```
('payload', 'timestamp', 'type')
```
Com `type == "event_msg"` e `payload.type == "token_count"`, o corpo relevante é:
```
payload.info.total_token_usage{input_tokens, cached_input_tokens, cache_write_input_tokens,
                                output_tokens, reasoning_output_tokens, total_tokens}
payload.info.last_token_usage{... mesmas chaves ...}
payload.info.model_context_window
payload.rate_limits{
  limit_id, limit_name,
  primary{used_percent, window_minutes, resets_at},
  secondary{used_percent, window_minutes, resets_at} | null,
  credits{has_credits, unlimited, balance},
  individual_limit, spend_control_reached, plan_type, rate_limit_reached_type
}
```
- Campo `timestamp` no nível da linha, mesmo formato ISO 8601 UTC (`"2026-08-21T11:40:17.467Z"`).
- 481 linhas com `rate_limits` só no arquivo mais recente amostrado — é o evento mais comum do log.

## 3. Confirmação: `rate_limits` com `used_percent` existe no Codex

Confirmado. Estrutura real observada em 3 sessões diferentes:
```json
"rate_limits": {
  "primary":   {"used_percent": 65.0, "window_minutes": 300,   "resets_at": 1787796268},
  "secondary": {"used_percent": 26.0, "window_minutes": 10080, "resets_at": 1788362435}
}
```
- `primary.window_minutes = 300` → **janela de 5 horas** (300 min = 5h). Mapeia para `codex.session_pct`.
- `secondary.window_minutes = 10080` → **janela de 7 dias** (10080 min = 7 dias). Mapeia para `codex.week_pct`.
- Em sessões mais antigas (antes de uma mudança de formato do próprio Codex, aparentemente), só existe `primary` com `window_minutes = 10080` e `secondary = null` — ou seja, o esquema de duas janelas é recente. O coletor trata `primary`/`secondary` dinamicamente por `window_minutes` (300 → sessão, 10080 → semana) em vez de assumir posição fixa, para não quebrar com logs antigos.

**DESVIO DO PEDIDO ORIGINAL (registrado, não inventado):** o campo chama-se `resets_at` (epoch Unix em **segundos**, timestamp absoluto do reset), e **não** `resets_in_seconds` (duração) como estava especificado na tarefa. Decisão: o coletor calcula `resets_in_seconds = resets_at - now_utc` internamente e expõe no contrato JSON como `reset_min` (minutos até o reset, arredondado), já convertendo. Isso mantém o contrato JSON estável sem inventar um campo de origem que não existe.

## 4. Claude Code não tem percentual de cota — decisão tomada

Confirmado: não existe percentual de cota gravado pelo Claude Code em nenhuma linha do JSONL (nem em `message`, nem em nenhum evento de nível superior como `isSnapshotUpdate`, `atis`, `aiTitle`, etc. — inspecionados via schema completo).

Decisão (conforme regra dura da tarefa — não inventar campo, adaptar ao que existe):
- O coletor soma `input_tokens + cache_creation_input_tokens + cache_read_input_tokens + output_tokens` de todas as linhas com `message.usage`, filtrando pelo `timestamp` da linha, em duas janelas rolantes:
  - **Sessão**: últimas 5 horas (`now_utc - 5h`), para paridade conceitual com `codex.session_pct`.
  - **Semana**: últimos 7 dias (`now_utc - 7d`).
- O percentual é `soma_tokens_janela / limite_calibrável * 100`, limitado a `[0, 100]`.
- Limite calibrável fica em `collector/limits.json` (ver seção de calibração no próprio arquivo — regra de três a partir do consumo observado e do plano contratado, já que a Anthropic não expõe o teto exato via log local).
- `claude.reset_min`: como não há `resets_at` nem duração de reset no log do Claude Code, o valor é **calculado**, não lido: para a janela de sessão, é o tempo até completar 5h desde o token mais antigo ainda dentro da janela; para a semana, análogo com 7 dias. Se a janela estiver vazia (sem uso), `reset_min = null`.

Valores plausíveis obtidos somando os 11 arquivos reais no momento da validação (2026-08-28, uso do dia — mostra que o parser funciona e produz números plausíveis, sem vazar prompt):
```
janela 5h  -> tokens de entrada (input+cache): 1.156.098 | tokens de saída: 8.490
janela 7d  -> tokens de entrada (input+cache): 1.156.098 | tokens de saída: 8.490
```
(Os dois totais coincidem porque toda a atividade recente do usuário caiu dentro das últimas 5h no momento da amostra — comportamento esperado, não é bug.)

## 5. Formato de timestamp e timezone

Ambas as fontes usam **ISO 8601 UTC com sufixo `Z`** no campo `timestamp` de cada linha (`"2026-08-26T21-38-31"` no nome de arquivo do Codex usa `-` por ser nome de arquivo, mas o campo `timestamp` interno usa `:` e `Z` normalmente). O coletor:
- Faz `datetime.fromisoformat(ts.replace('Z', '+00:00'))` para obter `datetime` timezone-aware.
- Compara tudo em UTC (`datetime.now(timezone.utc)`), nunca em horário local — evita bug de fuso ao rodar o servidor.
- `resets_at` do Codex é epoch em segundos (UTC por definição) — convertido com `datetime.fromtimestamp(ts, tz=timezone.utc)`.

## 6. O que ficou ABERTO (não resolvido, não inventado)

- **Limite exato de tokens do plano Claude Code por janela de 5h/7d**: a Anthropic não expõe esse número em nenhum arquivo local. `collector/limits.json` traz um valor de partida com instrução de calibração manual por regra de três (comparando o percentual mostrado pela própria CLI, se houver, contra o total somado pelo coletor no mesmo instante). **Ação pendente do usuário**: ajustar `limits.json` observando `/usage` (ou equivalente) na CLI do Claude Code real e comparando com o total bruto do coletor.
- **Esquema de `rate_limits` sem `secondary`**: sessões antigas do Codex (antes de ~2026-08-21) só têm janela semanal (`window_minutes=10080`) em `primary`, sem janela de 5h. O coletor usa apenas a sessão/arquivo mais recente por `mtime`, então isso não afeta o resultado atual, mas fica registrado como variação de esquema possível caso arquivos antigos sejam processados.
- **Rotação/retenção dos logs**: não foi validado se `~/.claude/projects` e `~/.codex/sessions` têm rotação automática (arquivos muito antigos podem inflar a varredura ao longo do tempo). O coletor limita a soma pela janela de tempo, mas a varredura de arquivos não tem corte por idade — se isso virar problema de performance, adicionar filtro por `mtime` do arquivo antes de abri-lo.
- **Múltiplas contas/orgs no mesmo `~/.claude`**: o arquivo de sessão traz `ownerAccountUuid`/`ownerOrganizationUuid`, mas não foi validado o comportamento com mais de uma conta ativa simultaneamente — assume-se conta única (uso real do usuário).

## 7. Revisão em 2026-08-30: fonte primária e expiração

- O endpoint de uso do Claude autenticado devolveu `five_hour` e `seven_day`
  com `utilization` e `resets_at`.
- O endpoint de uso do Codex devolveu `primary_window` (18.000 s) e
  `secondary_window` (604.800 s), com `used_percent` e `reset_at`.
- Essas fontes agora são primárias; os JSONL permanecem como fallback e fonte
  de contagem de tokens.
- Um `rate_limits` local cujo `resets_at` já passou é descartado. Esse teste de
  regressão elimina o estado persistente `54% / renova 0m`.
- A projeção requer no mínimo três amostras e dez minutos de histórico. Usa a
  taxa positiva da última hora e nunca projeta além do reset da janela.
