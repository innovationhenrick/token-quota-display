# Atualização OTA

Depois da gravação inicial da versão OTA-capable, o ESP32 consulta o coletor pelo
Wi-Fi após cada sincronização inicial e, depois, a cada seis horas. Se o manifesto
tiver um `version_code` maior, baixa o binário, confere tamanho e MD5, grava a
partição OTA alternativa e reinicia.

Para publicar uma versão nova:

1. Incremente `FIRMWARE_VERSION_CODE` em `firmware/token_quota_display.ino`.
2. Compile e publique:

```sh
sh collector/build_publish_ota.sh
```

O script publica em `~/Library/Application Support/TokenQuotaDisplay/firmware/`.
O LaunchAgent já serve o manifesto e o binário na mesma porta do coletor.

O primeiro binário OTA-capable exige uma gravação USB/JTAG. A partir dele, as
próximas versões podem ser distribuídas pelo Wi-Fi da rede local.
