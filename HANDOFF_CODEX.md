# Handoff: bug de renderização do arco LVGL no Token Quota Display

## Contexto do projeto
Painel físico ESP32-2424S012 (ESP32-C3-MINI-1U-N4X, RISC-V, 4MB flash, sem PSRAM) com tela IPS
redonda 240x240 GC9A01, mostrando cotas de token do Claude Code e Codex CLI via dois arcos
concêntricos (LVGL 9.2.2 + Arduino_GFX + ArduinoJson).

Projeto completo em:
`/Users/henricksantana/Library/CloudStorage/GoogleDrive-itlhenricksantana@gmail.com/Meu Drive/[02] Work/[03] itlmkt/cubo/token-quota-display/`

- `collector/quota_server.py` — servidor HTTP stdlib, testado e funcionando (`curl http://localhost:8787/quota` retorna JSON válido, 13 testes unitários passam).
- `firmware/token_quota_display.ino` — **firmware com bug ativo, em investigação, com código de DEBUG ainda presente** (ver abaixo).
- `firmware/lv_conf.h`, `firmware/secrets.h` (já preenchido com credenciais reais), `firmware/secrets.h.example`.

## Ambiente de build (macOS, sem depender da Arduino IDE GUI)

A Arduino IDE 2.3.10 tem um bug de UI que não sincroniza a lista de placas mesmo com o core
instalado corretamente (confirmado via `arduino-cli` que tudo está ok por baixo). **Não perca
tempo tentando consertar a IDE gráfica — use o `arduino-cli` embutido nela, direto pelo terminal.**

```bash
CLI=/Applications/"Arduino IDE.app"/Contents/Resources/app/lib/backend/resources/arduino-cli
CFG=~/.arduinoIDE/arduino-cli.yaml
FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,JTAGAdapter=default,UploadSpeed=921600"
```

Config já ajustada (`~/.arduinoIDE/arduino-cli.yaml`):
```yaml
board_manager:
    additional_urls:
        - https://espressif.github.io/arduino-esp32/package_esp32_index.json
```
(A URL que a documentação oficial antiga cita, com `gh-storage`, retorna 404 — não usar.)

Bibliotecas já instaladas via `arduino-cli lib install`:
- `GFX Library for Arduino` 1.6.7 (moononournation)
- `lvgl` 9.2.2
- `ArduinoJson` 7.4.3

`lv_conf.h` do projeto já copiado para `~/Documents/Arduino/libraries/lv_conf.h` (é onde o LVGL
o procura por padrão, `../../lv_conf.h` relativo a `lvgl/src/`).

**IMPORTANTE — dois arquivos foram deletados da instalação local da lib `lvgl`** (não existem no
projeto, só na instalação em `~/Documents/Arduino/libraries/lvgl/`), porque quebravam a
compilação para RISC-V (vazavam `typedef`s pro assembler, arquivos exclusivos de ARM
Helium/NEON, nunca usados no C3):
```
~/Documents/Arduino/libraries/lvgl/src/draw/sw/blend/helium/lv_blend_helium.S
~/Documents/Arduino/libraries/lvgl/src/draw/sw/blend/neon/lv_blend_neon.S
```
Se reinstalar a lib lvgl do zero, **apagar esses dois arquivos de novo** antes de compilar, ou a
compilação vai falhar com `Error: unrecognized opcode` no assembler.

`lv_conf.h` também precisou de `#define LV_USE_SCALE 0` explícito (não estava no arquivo
original) porque o widget Scale depende de `LV_USE_LINE`, que está desligado — sem isso dá erro
de linkedição por função não declarada (`lv_line_get_point_count` etc.).

### Comando de build usado (funciona, compila com sucesso hoje):
```bash
rm -rf /tmp/build_token_quota_display
mkdir -p /tmp/build_token_quota_display/token_quota_display
cp "/caminho/do/projeto/firmware/token_quota_display.ino" /tmp/build_token_quota_display/token_quota_display/
cp "/caminho/do/projeto/firmware/secrets.h" /tmp/build_token_quota_display/token_quota_display/

CLI=/Applications/"Arduino IDE.app"/Contents/Resources/app/lib/backend/resources/arduino-cli
CFG=~/.arduinoIDE/arduino-cli.yaml
SKETCH="/tmp/build_token_quota_display/token_quota_display"
rm -rf ~/Library/Caches/arduino/sketches/*   # importante: cache de build pode ficar inconsistente

"$CLI" --config-file "$CFG" compile \
  --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,JTAGAdapter=default,UploadSpeed=921600" \
  "$SKETCH"

"$CLI" --config-file "$CFG" upload -p /dev/cu.usbmodem3101 \
  --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,JTAGAdapter=default,UploadSpeed=921600" \
  "$SKETCH"
```

**Nota**: o `arduino-cli` exige que o `.ino` principal tenha o mesmo nome da pasta que o contém —
por isso a cópia para `/tmp/build_token_quota_display/token_quota_display/token_quota_display.ino`
em vez de compilar direto na pasta `firmware/` do projeto (que se chama `firmware`, não
`token_quota_display`).

### Monitor serial (sem depender de pyserial, que não está instalado)
```bash
rm -f /tmp/serial_raw.log
(cat /dev/cu.usbmodem3101 > /tmp/serial_raw.log 2>&1 &)
sleep 70   # tempo suficiente pra pegar boot + pelo menos 1 ciclo de poll de 60s
pkill -f "cat /dev/cu.usbmodem3101" 2>/dev/null
cat /tmp/serial_raw.log
```
(`arduino-cli monitor` em background não funcionou de forma confiável — usar `cat` direto na
porta serial, que funcionou bem.)

## Estado confirmado por log real (não suposição)

Com o firmware já corrigido de vários bugs reais (ver seção seguinte), o log serial confirma:
```
Wi-Fi conectado, IP: 192.168.15.22
Buscando quota em: http://192.168.15.11:8787/quota
Código HTTP retornado: 200
Parseado -> claude:100% codex:97%
atualizar_ui() chamado
```
Ou seja: **Wi-Fi ok, HTTP ok, parse do JSON ok, a função que atualiza a UI é chamada com os
valores corretos.** O problema é 100% isolado na camada de renderização LVGL/display.

## Bugs já corrigidos nesta sessão (não reabrir)

1. **API do LVGL 8.x usada por engano** — o `.ino` original usava `lv_disp_drv_t`,
   `lv_disp_draw_buf_init`, `lv_disp_drv_register` (API antiga, removida no LVGL 9.2). Corrigido
   para a API nativa 9.x: `lv_display_create()`, `lv_display_set_flush_cb()`,
   `lv_display_set_buffers()`, `lv_display_flush_ready()`, `lv_screen_active()` (em vez de
   `lv_scr_act()`).
2. **`BLACK` não existe** na versão instalada do Arduino_GFX — usar `RGB565_BLACK`.
3. **Toolchain RISC-V com arquivos `.S` ARM-only** — ver seção acima (helium/neon deletados).
4. **`LV_USE_SCALE` faltando no `lv_conf.h`** — ver acima.
5. **SSID errado**: a rede visível era "HQ-5GHz" (banda 5GHz, que o ESP32 NÃO suporta — só tem
   rádio 2.4GHz). A rede certa é `HQ` (2.4GHz, mesma senha). Já corrigido em `secrets.h`.
6. **Bug de lógica no `loop()`**: o poll original marcava `g_last_poll_ms` como "consumido" MESMO
   quando falhava por falta de Wi-Fi, fazendo a primeira tentativa (que cai antes do Wi-Fi
   conectar, no boot) desperdiçar o ciclo inteiro de 60s. Corrigido: só marca o ciclo como
   consumido quando `WiFi.status() == WL_CONNECTED`.

## O BUG ATUAL (não resolvido) — foco da investigação

**Sintoma**: os dois arcos (`arc_claude`, `arc_codex`) renderizam a **trilha de fundo**
(`LV_PART_MAIN`, cinza escuro `0x333333`, largura 14px) normalmente — visível na tela redonda.
Mas o **indicador de valor** (`LV_PART_INDICATOR`, que deveria preencher proporcionalmente ao
valor com a cor do semáforo verde/âmbar/vermelho) **nunca aparece**, mesmo com os dados corretos
chegando e `lv_arc_set_value()` + `lv_obj_set_style_arc_color(..., LV_PART_INDICATOR)` sendo
chamados.

**Testes de isolamento já feitos (resultados confirmados pelo usuário olhando a tela física)**:

| Teste | O que foi feito | Resultado |
|---|---|---|
| A | Fluxo completo normal (HTTP → JSON → `atualizar_ui()`) | Trilha aparece, indicador NUNCA aparece |
| B | `lv_arc_set_value(arc_claude, 50)` **fixo, direto em `construir_ui()`**, antes do primeiro `loop()` rodar | **Funciona!** Meio círculo verde aparece |
| C | `lv_obj_set_style_arc_color(..., LV_PART_INDICATOR)` + `lv_obj_set_style_arc_opa(..., LV_OPA_COVER, LV_PART_INDICATOR)` definidos já em `construir_ui()` (cor verde inicial) | Não resolveu — indicador ainda não aparece no fluxo normal |
| D | `lv_obj_invalidate(arc_claude)` explícito logo após `lv_arc_set_value`/`lv_obj_set_style_arc_color` em `atualizar_ui()` | Não resolveu |
| E | Forçar `lv_arc_set_value(arc_claude, 80)` **de dentro do `loop()`, 5 segundos após o boot** (sem HTTP nenhum envolvido — só `millis() > 5000`) | **Também falhou** — indicador não aparece |
| F | Aumentar `LVGL_BUF_LINES` de 40 para 100 (buffer parcial maior) | Teste E foi repetido com esse buffer maior — **resultado ainda não confirmado pelo usuário quando a sessão foi interrompida** |

**Conclusão do teste E**: o bug **não é específico do fluxo HTTP/JSON** (não é corrupção de heap
pela stack do HTTPClient, não é timing de rede). É genérico: **qualquer mudança de valor/cor do
arco feita a partir de uma iteração do `loop()` que não seja a primeira (antes do primeiro
`lv_timer_handler()` ter rodado) falha em aparecer visualmente**, mesmo que `lv_arc_set_value`
tecnicamente "funcione" (o valor interno muda, confirmado pelos logs de Serial).

**Teste F ainda pendente de resposta do usuário** — foi a última ação antes do handoff: aumentei
`LVGL_BUF_LINES` de 40 para 100 linhas (buffer parcial maior, ainda não full-frame — full-frame
de 240 linhas = 115.200 bytes não coube, dava overflow de DRAM de 6.384 bytes) e regravei com o
mesmo teste E (forçar arco vermelho aos 5s via `loop()`, sem HTTP). **O usuário não confirmou o
resultado desse teste específico** antes de pedir o handoff.

## Hipóteses ainda não testadas, para investigar a seguir

1. **Buffer parcial vs. modo full-frame de verdade**: o teste F com 100 linhas ficou pendente.
   Se ainda falhar, vale tentar reduzir RAM em outro lugar (ex.: desligar mais widgets no
   `lv_conf.h`, ou usar `LV_COLOR_DEPTH` diferente) para caber o buffer full-frame (240 linhas,
   115.200 bytes) e testar `LV_DISPLAY_RENDER_MODE_FULL` de verdade — isso eliminaria de vez a
   hipótese de bug no mecanismo de "flush por regiões" do buffer parcial.

2. **`lv_timer_handler()` não sendo chamado com frequência suficiente após a primeira mudança**:
   o `loop()` atual chama `lv_timer_handler()` uma vez por iteração, seguido de `delay(5)`. Pode
   valer a pena chamar `lv_timer_handler()` MAIS DE UMA VEZ seguida logo após uma mudança de
   valor (ex.: um `for` de 2-3 chamadas), para garantir que o LVGL processe a invalidação em
   múltiplos passes se for isso que ele espera.

3. **`lv_tick` nunca é alimentado — CONFIRMADO via `grep -n "lv_tick" token_quota_display.ino`,
   zero ocorrências no arquivo atual.** O `.ino` não chama `lv_tick_inc()` nem
   `lv_tick_set_cb()` em nenhum lugar. Isso é crítico no LVGL: sem informar quanto tempo passou
   a cada iteração, o LVGL não sabe quando processar timers/animações/redraws internos — inclui
   o `disp->refr_timer` (criado internamente por `lv_display_create`, período
   `LV_DEF_REFR_PERIOD = 30ms`), que sem tick não tem como saber que já é hora de rodar de novo
   depois do primeiro ciclo síncrono inicial. **Esta é a hipótese mais forte ainda não testada**,
   e explica coerentemente por que um valor definido ANTES do primeiro `lv_timer_handler()`
   (teste B) aparece, mas qualquer mudança posterior (testes A e E) não é redesenhada.

   **Ação recomendada como primeiro passo, antes de qualquer outra hipótese**: adicionar no
   `setup()`, logo após `lv_init()`:
   ```cpp
   lv_tick_set_cb(millis);
   ```
   (LVGL 9 aceita diretamente uma função com assinatura `uint32_t(void)` como callback de tick;
   `millis()` do Arduino bate com essa assinatura.) Isso é o suspeito nº1 e ainda não foi testado
   nesta sessão de debug — teste isso primeiro, sozinho, antes de mexer em buffer ou em
   `lv_timer_handler()` múltiplo (hipótese 2), para isolar causa e efeito com clareza.

## Código de DEBUG que precisa ser revertido depois de resolver

No arquivo `firmware/token_quota_display.ino` (estado atual, 347 linhas), existem 3 blocos
marcados com comentário `DEBUG` que devem ser **removidos/revertidos** assim que o bug for
corrigido:

1. Linha ~41: `#define LVGL_BUF_LINES 100 // DEBUG: buffer maior...` — reverter para `40` (ou o
   valor que a correção final exigir; documentar a decisão em vez de deixar como estava).
2. Linhas ~320-326 (dentro do `loop()`): bloco `DEBUG: testar se o arco atualiza fora do fluxo
   HTTP` inteiro — remover.
3. Também revisar se sobrou algum `Serial.print` de debug que deveria ser removido ou mantido
   como log leve (os prints de "Wi-Fi conectado", "Buscando quota em...", "Código HTTP
   retornado", "Parseado ->", "atualizar_ui() chamado" foram adicionados nesta sessão para
   diagnóstico — decidir se ficam como log permanente de baixo custo ou se são removidos para
   produção; se ficarem, considerar deixar só os essenciais).

Também reverter, se a hipótese do tick resolver o problema sem precisar delas:
- `lv_obj_invalidate(arc_claude)` / `lv_obj_invalidate(arc_codex)` em `atualizar_ui()` (linhas
  ~166 e ~171) — foram adicionadas como tentativa de fix, podem não ser mais necessárias.
- As duas linhas de `lv_obj_set_style_arc_color(..., LV_PALETTE_GREEN, LV_PART_INDICATOR)` +
  `lv_obj_set_style_arc_opa(..., LV_OPA_COVER, LV_PART_INDICATOR)` adicionadas em
  `construir_ui()` para ambos os arcos — avaliar se ainda fazem sentido como "cor inicial padrão
  antes do primeiro dado real chegar" (provavelmente sim, é boa prática, só não é a causa raiz).

## Critérios de aceite ainda não validados (retomar depois do fix)

Do escopo original do projeto:
1. ✅ `curl -s http://localhost:8787/quota` retorna JSON válido no contrato — confirmado.
2. ✅ Testes do coletor passam (13/13) — confirmado.
3. ✅ Firmware compila para ESP32C3 e cabe na partição (65% de 1.966.080 bytes) — confirmado.
4. ❌ **Arcos refletem o JSON visualmente** — bloqueado pelo bug acima. Estado de falha (label de
   erro) funciona (testado indiretamente: "Sem Wi-Fi" aparecia antes da correção do SSID).

## Arquivos de referência no projeto (para reler o histórico completo se precisar)
- `token-quota-display/VALIDATION.md` — Fase 1, esquemas reais de log do Claude Code e Codex.
- `token-quota-display/README.md` — passo a passo de flash/troubleshooting (**desatualizado
  quanto ao bug da IDE gráfica** — a seção de Arduino IDE lá assume que a IDE funciona
  normalmente; na prática usamos `arduino-cli` direto, como documentado aqui).
