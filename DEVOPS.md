# Fluxo DevOps

Este projeto usa um fluxo pequeno, baseado em pull requests e GitHub Actions. A ideia é manter o `main` sempre publicável sem transformar o repositório em uma infraestrutura difícil de operar.

## Fluxo de uma alteração

```text
main atualizado
      │
      ├─ branch feat/* ou fix/*
      │
      ├─ commit Conventional Commits em português
      │
      ├─ pull request para main
      │       └─ CI: Python, JSON e higiene de segredos
      │
      └─ merge aprovado → main → tag vX.Y.Z (opcional)
                                      └─ release de fonte
```

### 1. Criar uma branch

```bash
git switch main
git pull --ff-only origin main
git switch -c feat/nome-curto
```

Use `feat/` para funcionalidade e `fix/` para correção. Não faça push direto em `main` para mudanças de produto.

### 2. Commitar

Use mensagens curtas no padrão Conventional Commits:

```text
feat(firmware): destacar modelo ativo
fix(collector): preservar reset semanal expirado
docs(devops): documentar fluxo de release
```

Não inclua `firmware/secrets.h`, tokens, cookies, dumps, logs com prompts ou dados pessoais.

### 3. Abrir o pull request

```bash
git push -u origin feat/nome-curto
gh pr create --base main --fill
```

O PR deve explicar o que mudou, como validar, risco e rollback. A Action `CI` precisa terminar verde antes do merge.

## O que a CI verifica

`.github/workflows/ci.yml` roda em todo PR para `main`, em todo push para `main` e manualmente:

- compila a sintaxe Python do coletor;
- valida `collector/limits.json` como JSON;
- executa os testes unitários sintéticos do coletor;
- impede que `firmware/secrets.h` ou arquivos temporários entrem no Git.

O firmware continua sendo compilado e gravado localmente, pois exige a placa, as bibliotecas Arduino/LVGL e o arquivo privado `secrets.h`. O procedimento está em [HANDOFF_CODEX.md](HANDOFF_CODEX.md). A CI não tenta acessar hardware físico nem credenciais.

## Merge e proteção recomendada

No GitHub, habilite em `Settings → Branches → Branch protection` para `main`:

1. exigir pull request antes do merge;
2. exigir o check `CI / Coletor Python` e `CI / Higiene do repositório`;
3. exigir branch atualizada antes do merge;
4. desabilitar push force e exclusão da branch;
5. preferir **Squash and merge**.

Essa configuração é uma decisão administrativa do repositório e deve ser revisada antes de ser aplicada a uma equipe maior.

## Releases

Quando uma versão estiver validada no hardware:

```bash
git switch main
git pull --ff-only origin main
git tag -a v1.0.0 -m 'release: primeira versão estável'
git push origin v1.0.0
```

A tag `vX.Y.Z` dispara `.github/workflows/release.yml`, que publica um tarball da fonte e seu checksum SHA-256. O pacote não contém `firmware/secrets.h` porque esse arquivo nunca é rastreado.

Para uma atualização OTA real, siga [OTA.md](OTA.md) e publique o binário pelo coletor local. A release do GitHub é o registro versionado da fonte; ela não substitui a gravação USB inicial nem o servidor OTA da LAN.

## Rollback

- **Código:** reverta o PR ou abra `fix/rollback-<versão>`; não reescreva o histórico de `main`.
- **Release:** marque a versão anterior como referência e publique uma nova tag de correção; tags existentes não devem ser reutilizadas.
- **Firmware:** use o binário anterior conhecido no fluxo OTA ou grave-o por USB, conforme [HANDOFF_CODEX.md](HANDOFF_CODEX.md).

## Rotina curta para o dia a dia

```bash
git switch main
git pull --ff-only origin main
git switch -c feat/minha-alteracao
# editar, revisar o diff e commitar
git diff --check
git add README.md collector firmware
git commit -m 'tipo(escopo): descrição curta'
git push -u origin HEAD
# abrir PR e aguardar a CI
```

