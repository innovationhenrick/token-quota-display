# Cotas semanais externas

## Gemini via Antigravity CLI

O Antigravity CLI envia um JSON estruturado para o comando configurado em
`statusLine`. A instalação da ponte é feita por:

```bash
sh collector/install_antigravity_bridge.sh
```

A ponte grava somente os buckets de quota em:

```text
~/Library/Application Support/TokenQuotaDisplay/antigravity-quota.json
```

O coletor usa o bucket `gemini-weekly` e preserva:

- `remaining_fraction` (fonte do percentual restante);
- `reset_time` ou `reset_in_seconds`;
- idade da amostra e indicador `stale`.

O percentual exibido como uso é sempre calculado como `100 - restante`.

Além da `statusLine`, o coletor executa o comando oficial `/usage` no máximo a
cada cinco minutos quando o snapshot está ausente ou antigo. Essa atualização
é feita em segundo plano e não envia prompts nem consome tokens de conversa.
Se o Antigravity estiver fechado ou sem autenticação, o painel preserva o
último valor válido e sinaliza os dados como antigos, em vez de exibir `0%`.

## Grok

O contrato local para o Grok é o mesmo, em:

```text
~/Library/Application Support/TokenQuotaDisplay/grok-quota.json
```

O snapshot deve conter apenas dados de uso, sem cookies, tokens ou conteúdo de
conversas. O formato aceito é:

```json
{
  "updated_epoch": 0,
  "weekly": {
    "used_pct": 6,
    "remaining_pct": 94,
    "reset_epoch": 0
  }
}
```

Enquanto a fonte do Grok não publicar esse snapshot, o `/quota` retorna
`available: false` e `stale: true`, em vez de inventar um zero.
