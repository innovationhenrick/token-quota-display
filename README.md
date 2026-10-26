# Token Quota Display

> Um mostrador circular para enxergar, de relance, o consumo e o próximo reset das cotas de IA.

[![ESP32-C3](https://img.shields.io/badge/hardware-ESP32--C3-0b8f87?logo=espressif)](https://www.espressif.com/en/products/socs/esp32-c3)
[![LVGL 9](https://img.shields.io/badge/UI-LVGL%209-4051b5)](https://lvgl.io/)
[![Python 3.9+](https://img.shields.io/badge/collector-Python%203.9%2B-3776ab?logo=python&logoColor=white)](https://www.python.org/)
[![Status](https://img.shields.io/badge/status-experimental-f2a663)](VALIDATION.md)

O projeto combina um **ESP32-C3 com display redondo GC9A01/CST816D (240×240)** e um coletor local no macOS. O coletor consulta as fontes disponíveis para **Claude, Codex, Gemini/Antigravity e Grok**, normaliza os dados e entrega um JSON pequeno ao dispositivo pela rede Wi‑Fi.

O firmware mostra percentuais, janelas de 5 horas ou 7 dias, contagem regressiva até o reset, histórico recente e mascotes pixel-art. Nenhuma credencial ou conteúdo de prompt é enviado ao ESP32: somente o resumo agregado de uso atravessa a LAN.

## O que este projeto resolve

- **Visão imediata:** quatro anéis concêntricos deixam o percentual legível sem abrir uma aplicação.
- **Tempo até o reset:** os rótulos diagonais exibem a janela ativa e o countdown correspondente.
- **Múltiplos provedores:** Claude e Codex usam as cotas de sessão/semana; Gemini e Grok usam a janela semanal quando disponível.
- **Atualização automática:** o coletor mantém cache curto e o ESP32 faz polling periódico, sem exigir intervenção manual.
- **Operação segura:** credenciais ficam em `firmware/secrets.h`, ignorado pelo Git; o firmware recebe apenas o contrato `/quota`.

## Visão geral da arquitetura

```text
Claude / Codex / Antigravity / Grok
            │ endpoints, CLI e caches locais
            ▼
    collector/quota_server.py
      cache de 5 s + histórico
            │ HTTP GET /quota :8787
            ▼ Wi‑Fi 2,4 GHz
   ESP32-C3 + LVGL + GC9A01
```

O coletor roda como processo local ou LaunchAgent. O display descobre o Mac por mDNS e mantém um IP de fallback configurável para redes onde o mDNS não estiver disponível.

## Começo rápido

### 1. Inicie o coletor no Mac

```bash
cd token-quota-display/collector
python3 quota_server.py

# confirme o contrato antes de ligar o hardware
curl -s http://localhost:8787/quota | python3 -m json.tool
```

Para manter o processo ativo em segundo plano:

```bash
collector/install_service.sh
launchctl print gui/$(id -u)/com.rick.quotadisplay
```

O instalador copia o runtime para `~/Library/Application Support/TokenQuotaDisplay`, evitando as restrições do macOS para LaunchAgents executados diretamente no Google Drive.

### 2. Configure as credenciais do firmware

```bash
cp firmware/secrets.h.example firmware/secrets.h
```

Preencha a rede Wi‑Fi 2,4 GHz e, se necessário, o IP de fallback do Mac. Para descobrir o IP local:

```bash
ipconfig getifaddr en0
```

`firmware/secrets.h` é local e não deve ser commitado.

### 3. Compile e grave o ESP32-C3

Use Arduino IDE ou `arduino-cli` com:

| Opção | Valor |
|---|---|
| Placa | `ESP32C3 Dev Module` |
| USB CDC On Boot | `Enabled` |
| CPU | `160MHz` |
| Flash | `4MB`, `QIO`, `Minimal SPIFFS` |
| Upload | `921600` |
| Porta | `/dev/cu.usbmodem3101` (exemplo) |

Dependências do Library Manager: **GFX Library for Arduino**, **LVGL 9.x** e **ArduinoJson 7.x**. O `firmware/lv_conf.h` já reduz fontes e widgets para caber no C3 sem PSRAM.

## Contrato da API

O endpoint é `GET http://<ip-do-mac>:8787/quota`:

```json
{
  "claude": {
    "session_pct": 12,
    "week_pct": 8,
    "session_reset_min": 132,
    "week_reset_min": 1530,
    "session_tokens": 123456,
    "projection_min": 98,
    "projected_pct_at_reset": 18
  },
  "codex": { "session_pct": 4, "week_pct": 2 },
  "gemini": {
    "week_pct": 5,
    "week_remaining_pct": 95,
    "week_reset_min": 6120,
    "quota_kind": "weekly"
  },
  "grok": {
    "week_pct": 14,
    "week_remaining_pct": 86,
    "week_reset_min": 8280,
    "quota_kind": "weekly"
  },
  "updated": 1730000000
}
```

Campos ausentes ou fontes indisponíveis são representados como `null`/`available: false`; o firmware preserva a última leitura válida e sinaliza dados antigos. O schema detalhado e a origem de cada campo estão em [VALIDATION.md](VALIDATION.md).

## Interface no display

- **Overview:** quatro anéis concêntricos, rótulos fixos de `100%`, `25%`, `50%` e `75%`, modelo ativo no centro e percentual em destaque.
- **Diagonais externas:** siglas curtas (`5H`/`7D`) e tempos de reset, sem texto curvo letra a letra; isso reduz RAM e melhora a leitura no círculo.
- **Rotação:** Claude, Codex, Grok e Gemini alternam a cada 8 segundos; o ponto de paginação acompanha o modelo ativo.
- **Foco visual:** o anel ativo mantém a cor cheia e os outros três ficam suavizados para reduzir halo e facilitar a leitura.
- **Paleta:** Claude âmbar, Codex ciano, Grok branco-gelo e Gemini azul-celeste; textos usam branco suave em vez de branco estourado.
- **Detalhes:** deslize para os lados para consultar as telas de 5 horas e 7 dias de cada modelo.
- **Estados:** mascotes reagem ao nível de uso; alertas aparecem quando uma janela cruza 25%, 50%, 70% ou 100%.

## Fontes de dados e fallback

O coletor tenta primeiro as fontes de uso autenticadas e faz fallback para logs/cache locais quando uma chamada falha. O Claude pode ser calibrado em `collector/limits.json`; o Codex expõe `used_percent` em `rate_limits` e não usa esse arquivo. Gemini/Antigravity e Grok têm cache próprio e marcam a leitura como antiga quando o CLI ou a fonte externa não está disponível.

Os endpoints de terceiros não são contratos públicos estáveis. Por isso o servidor expõe um schema estável para o firmware e concentra as adaptações em um único processo Python.

## Operação e troubleshooting

| Sintoma | Verificação rápida |
|---|---|
| `curl` local não retorna | Rode `python3 collector/quota_server.py` e veja o erro no terminal. |
| Outro dispositivo não acessa a porta 8787 | Libere Python/8787 no firewall do macOS e confirme o IP da interface ativa. |
| ESP32 fica em “Sem Wi‑Fi” | Confira SSID/senha, rede 2,4 GHz e `firmware/secrets.h`. |
| ESP32 fica em “Erro HTTP” | Confira se o Mac está na mesma LAN e se o fallback/IP mDNS está correto. |
| Percentual do Claude parece incorreto | Recalibre `collector/limits.json` seguindo a seção de calibração do README e `VALIDATION.md`. |
| Tela liga, mas fica preta | Confirme backlight no pino 3, rotação GC9A01 e a alimentação USB. |

Logs do LaunchAgent ficam em `/tmp/com.rick.quotadisplay.out.log` e `/tmp/com.rick.quotadisplay.err.log`.

## Estrutura do projeto

```text
token-quota-display/
├── README.md                         # visão geral e quick start
├── VALIDATION.md                     # fontes, schema e decisões de validação
├── HANDOFF_CODEX.md                  # build/upload pelo CLI
├── OTA.md                            # publicação opcional de firmware
├── QUOTAS_EXTERNAS.md                # notas de Gemini/Antigravity e Grok
├── collector/
│   ├── quota_server.py               # servidor HTTP stdlib, GET /quota
│   ├── limits.json                   # limites calibráveis do Claude
│   ├── install_service.sh            # instala o LaunchAgent
│   ├── publish_ota.sh                 # publica binário/manifest para OTA
│   └── test_*.py                     # fixtures e testes do coletor
└── firmware/
    ├── token_quota_display.ino       # Arduino + LVGL + Arduino_GFX
    ├── arduino_entrypoint.ino        # entrypoint para o fluxo Arduino CLI
    ├── lv_conf.h                     # configuração LVGL para o C3
    └── secrets.h.example             # template sem credenciais
```

## Documentação complementar

- [VALIDATION.md](VALIDATION.md) — como cada fonte foi validada e quais campos são confiáveis.
- [HANDOFF_CODEX.md](HANDOFF_CODEX.md) — compilação, upload e verificação serial.
- [OTA.md](OTA.md) — fluxo de atualização remota do firmware.
- [QUOTAS_EXTERNAS.md](QUOTAS_EXTERNAS.md) — limitações e fallback das cotas externas.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) — bibliotecas e avisos de terceiros.

## Roadmap

- [x] Coletor local com contrato único e cache curto.
- [x] Overview circular com foco no modelo ativo.
- [x] Claude, Codex, Gemini/Antigravity e Grok no mesmo payload.
- [x] Serviço persistente no macOS e publicação OTA preparada.
- [ ] Tela de histórico diário/semanal com tendência de consumo.
- [ ] Descoberta automática do coletor na LAN sem IP de fallback.
- [ ] Testes de contrato executados no CI do repositório.

## Contribuindo

Abra uma issue descrevendo o provedor, a fonte de dados e um exemplo anonimizado do payload. Para mudanças no firmware, inclua a placa, a revisão do display e uma foto ou captura da tela afetada. Nunca envie tokens, cookies, `secrets.h`, logs com prompts ou dados pessoais.

## Licença

O repositório ainda não declara uma licença de distribuição. Antes de reutilizar o código em outro produto, alinhe a licença e os avisos de terceiros com os mantenedores.
