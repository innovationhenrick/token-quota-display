# Token Quota Display

Painel físico redondo (ESP32-C3 + GC9A01/CST816D 240x240) para acompanhar as
cotas do **Claude Code**, **Codex CLI**, **Gemini** e **Grok** em tempo real.
Mostra as janelas de 5h e 7 dias, countdown de reset, tokens, projeção e
mascotes pixel-art reativos.
Nenhuma credencial é enviada ao firmware: apenas o JSON agregado trafega na LAN.

Ver [VALIDATION.md](VALIDATION.md) para o levantamento completo dos caminhos
de log, esquemas de campo e decisões tomadas antes de codar.

## Arquitetura

```
Claude/Codex usage endpoints ──┐
                               ├─> collector/quota_server.py ──> GET /quota ──Wi-Fi──> ESP32-C3
logs locais (fallback/tokens) ─┘       cache 5s + histórico
```

Contrato JSON estável, servido em `http://<ip-do-mac>:8787/quota`:
```json
{"claude":{"session_pct":int,"week_pct":int,
 "session_reset_min":int|null,"week_reset_min":int|null,
 "session_tokens":int,"projection_min":int|null,"projected_pct_at_reset":int|null},
 "codex":{"...":"mesmos campos"},
 "updated":epoch_seconds}
```

## 1. Coletor (Mac)

### Rodar manualmente
```bash
cd token-quota-display/collector
python3 quota_server.py
# em outro terminal:
curl -s http://localhost:8787/quota
```

### Rodar os testes
```bash
cd token-quota-display/collector
python3 -m unittest test_quota.py -v
```

### Fontes e fallback

O coletor consulta os endpoints de uso das contas já autenticadas no Claude
Code e Codex. Se uma chamada falhar, cai automaticamente nos logs locais. No
fallback do Claude, `collector/limits.json` continua sendo usado para estimar o
percentual. Os endpoints não são contratos públicos estáveis e podem mudar.

Para calibrar o fallback do Claude:

1. Abra a CLI do Claude Code e veja o percentual real de uso (comando de
   status/uso da sua versão).
2. No mesmo instante, rode `curl -s http://localhost:8787/quota` e anote o
   `session_pct`/`week_pct` retornado.
3. Regra de três: `novo_limite = limite_atual * percentual_real / percentual_mostrado`.
4. Edite `session_token_limit` / `week_token_limit` em `collector/limits.json`.

O Codex **não precisa de calibração** — ele já expõe `used_percent` pronto em
`rate_limits` (ver VALIDATION.md, seção 3).

### Instalar como serviço em background (LaunchAgent)

```bash
collector/install_service.sh

# verificar se está rodando
launchctl print gui/$(id -u)/com.rick.quotadisplay
curl -s http://localhost:8787/quota

# parar/desinstalar
launchctl bootout gui/$(id -u)/com.rick.quotadisplay
```

Logs do serviço: `/tmp/com.rick.quotadisplay.out.log` e `.err.log`.

O instalador copia o runtime para
`~/Library/Application Support/TokenQuotaDisplay`, evitando o bloqueio TCC do
macOS contra scripts executados pelo LaunchAgent dentro do Google Drive.

## 2. Firmware (ESP32-2424S012)

### Dependências (Arduino IDE — Library Manager)
- `GFX Library for Arduino` (moononournation/Arduino_GFX) — driver GC9A01
- `lvgl` (versão 9.x)
- `ArduinoJson` (versão 7.x)
- Core `esp32` da Espressif instalado (Boards Manager) — necessário para a
  placa "ESP32C3 Dev Module"

### Configuração da placa
Arduino IDE → Tools:
- Board: **ESP32C3 Dev Module**
- USB CDC On Boot: **Enabled**
- CPU Frequency: **160MHz**
- Flash Frequency: **80MHz**, modo **QIO**
- Flash Size: **4MB**
- Partition Scheme: **Minimal SPIFFS**
- Upload Speed: **921600**
- JTAG Adapter: **Disabled**
- Port: `/dev/cu.usbmodem3101`

### lv_conf.h
O `lv_conf.h` deste projeto já vem ajustado para o C3 sem PSRAM
(`LV_COLOR_DEPTH 16`, `LV_MEM_SIZE` reduzido, fontes 12/14/16/20 e somente os
widgets necessários). Copie-o para onde sua instalação
do LVGL espera encontrá-lo — normalmente ao lado da pasta `lvgl/` em
`Documents/Arduino/libraries/`, ou na raiz do sketch se sua versão da
biblioteca lê `lv_conf.h` local primeiro.

### secrets.h
```bash
cp firmware/secrets.h.example firmware/secrets.h
```
Edite `firmware/secrets.h` com o SSID/senha da rede 2.4GHz e a URL do
coletor. Para descobrir o IP local do Mac na LAN:
```bash
ipconfig getifaddr en0
```
`secrets.h` não deve ser commitado — adicione ao `.gitignore` do repositório
se ainda não estiver.

### Build e flash pelo CLI
1. Conecte o ESP32-2424S012 via USB.
2. Selecione a porta `/dev/cu.usbmodem3101` (USB CDC nativo do C3 — nenhum
   driver CH340 necessário).
Use os comandos de `arduino-cli` documentados em `HANDOFF_CODEX.md`; a pasta
temporária precisa se chamar `token_quota_display` e conter também `secrets.h`.

### Interface

- Tela central: quatro anéis concêntricos, modelo ativo, percentual e countdown.
- O modelo ativo alterna entre Claude, Codex, Grok e Gemini a cada 4 segundos.
- Os diagonais usam siglas curtas (`5H`/`7D`) e tempos de reset sem texto curvo,
  mantendo a leitura nítida no display circular.
- Swipe para a esquerda/direita: detalhes de 5h e 7 dias de cada modelo.
- O dashboard usa fundo azul-ardósia e uma paleta de baixo brilho para reduzir
  halos e facilitar a leitura.
- Clawd em silhueta pixel-art clássica; Codex usa uma marca técnica geométrica,
  pois não existe mascote oficial. Piscam/pulsam em estado normal, indicam
  esforço a partir de 70% e tremem em 100%.
- No resumo, Claude é âmbar, Codex é ciano, Grok é branco-gelo e Gemini é
  dourado suave. O anel ativo recebe destaque e os demais são suavizados.
- Alertas quando uma das janelas cruza 25%, 50%, 70% ou 100%; o primeiro poll
  após ligar apenas inicializa o estado e não dispara alertas antigos.

### Teste de ponta a ponta
1. Coletor rodando no Mac (`python3 quota_server.py` ou via LaunchAgent) e
   acessível na LAN (`curl -s http://<ip-do-mac>:8787/quota` de outro
   dispositivo na mesma rede, para confirmar que não é bloqueado por
   firewall).
2. ESP32 ligado e conectado à mesma rede 2.4GHz.
3. Em até 10s (intervalo de polling), os arcos devem preencher com os
   percentuais reais.
4. Desligue o coletor (`launchctl unload ...` ou `Ctrl+C` no processo
   manual) e confirme que a tela mostra o label de erro de HTTP em até 60s.
5. Corrompa temporariamente `limits.json` (ex.: apague uma chave) e confirme
   que o coletor não quebra — cai no valor default (ver `test_quota.py`).

## 3. Troubleshooting

| Sintoma | Causa provável | Ação |
|---|---|---|
| `curl` no Mac não retorna nada | Servidor não está rodando | `python3 quota_server.py` manualmente e observar erro no terminal |
| `curl` de outro dispositivo na LAN falha, mas `localhost` funciona | Firewall do macOS bloqueando porta 8787 | System Settings → Network → Firewall → permitir Python/porta 8787 |
| ESP32 não aparece em `/dev/cu.usbmodem*` | Cabo USB só de energia, ou placa não em modo correto | Trocar cabo (precisa de dados), verificar se `USB CDC On Boot = Enabled` está selecionado antes de compilar |
| Compilação falha por falta de memória / overflow de flash | Partition Scheme errado, ou bibliotecas extras habilitadas no `lv_conf.h` | Confirmar "Minimal SPIFFS"; manter os widgets desligados no `lv_conf.h` como entregue |
| Tela liga mas fica preta | Backlight (pino 3) não está em HIGH, ou driver GC9A01 com rotação/IPS errados | Conferir `pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);` no `setup()` |
| Label "Sem Wi-Fi" não some | SSID/senha errados em `secrets.h`, ou rede é 5GHz (C3 só suporta 2.4GHz) | Revisar `secrets.h`; confirmar que a rede é 2.4GHz |
| Label "Erro HTTP: conexao" | Coletor não acessível no IP configurado, ou IP do Mac mudou (DHCP) | Reconferir `ipconfig getifaddr en0` e atualizar `QUOTA_SERVER_URL` em `secrets.h` |
| `claude.session_pct` sempre 100% ou sempre 0% | `limits.json` descalibrado | Seguir o passo de calibração acima |
| Testes do coletor falham | Ambiente Python diferente do esperado (versão antiga sem `zoneinfo`/`fromisoformat` com `Z`) | Confirmar Python 3.9+ (o parser usa `str.replace('Z','+00:00')`, funciona desde 3.7, mas teste em 3.9+ é o alvo) |

## Estrutura do projeto

```
token-quota-display/
├── VALIDATION.md                       # Fase 1: validação dos logs reais
├── README.md                           # este arquivo
├── collector/
│   ├── quota_server.py                 # servidor HTTP stdlib, GET /quota
│   ├── limits.json                     # limites calibráveis (Claude)
│   ├── test_quota.py                   # testes com fixtures sintéticas
│   ├── install_service.sh               # instala runtime + LaunchAgent
│   └── com.rick.quotadisplay.plist     # template RunAtLoad + KeepAlive
└── firmware/
    ├── token_quota_display.ino         # Arduino + LVGL 9 + Arduino_GFX + ArduinoJson
    ├── lv_conf.h                       # config LVGL ajustada ao C3 sem PSRAM
    └── secrets.h.example                # template de credenciais (copiar para secrets.h)
```
