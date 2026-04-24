# Análise de compatibilidade do firmware de teste com o app atualizado

Este documento registra a auditoria técnica do firmware `IHM_final-main` para ESP32 DEVKIT V1 / ESP32-WROOM-32 e as mudanças feitas para alinhá-lo ao aplicativo mais atual, principalmente no novo fluxo de provisionamento e na operação com e sem internet.

## Resumo executivo

Antes desta revisão, o firmware de teste já estava **compatível com a parte cloud via MQTT** em boa parte do protocolo:

- tópicos MQTT padronizados
- payloads de `status`, `state`, `capabilities`, `commands` e `schedules`
- publicação de telemetria
- recepção de comandos
- capabilities dinâmicas
- agendamentos locais

O problema principal era outro: ele **não estava compatível com o novo onboarding do app**, porque faltavam:

- AP de provisionamento automático ao ligar
- endpoint local para receber `deviceId + SSID + senha`
- persistência desse provisionamento em NVS
- fallback HTTP local para o app quando a cloud estiver indisponível

Por isso, a atualização foi focada em **conectividade e provisionamento**, preservando a lógica já funcional da IHM.

## O que já estava compatível

Os pontos abaixo já estavam alinhados ou próximos do esperado pelo app atualizado:

- Board e framework corretos para a placa de teste:
  - `platform = espressif32`
  - `board = esp32dev`
  - `framework = espidf`
- MQTT cloud com:
  - subscribe em `commands`
  - subscribe em `schedules`
  - publish de `status`
  - publish de `state`
  - publish de `capabilities`
  - publish de `events/errors`
- Integração real dos comandos MQTT com a lógica existente da IHM via `ihm_mqtt_adapter`
- Schedules já persistidos e executados localmente
- Sincronização de horário para schedules
- Device ID persistente em `commcfg`
- Reaproveitamento seguro da lógica antiga da IHM em `main.c`

## O que não estava compatível

Os pontos abaixo impediam a compatibilidade real com o app atualizado:

### 1. Provisionamento principal do app

O app atualizado passou a usar como fluxo principal:

1. IHM liga
2. AP de provisionamento sobe automaticamente
3. usuário conecta o celular no AP da IHM
4. app envia:
   - `deviceId`
   - `wifiSsid`
   - `wifiPassword`

O firmware de teste antigo **não tinha**:

- AP automático
- `WIFI_MODE_AP` ou `WIFI_MODE_APSTA`
- servidor HTTP local
- endpoint `/api/v1/provisioning`
- função para salvar o provisionamento no NVS

### 2. Fallback local

O app atualizado já tem transporte local HTTP com:

- `GET /api/v1/ping`
- `GET /api/v1/status`
- `GET /api/v1/state`
- `GET /api/v1/capabilities`
- `POST /api/v1/commands`
- `GET /api/v1/schedules`
- `POST /api/v1/schedules`
- `GET /api/v1/diagnostics`

O firmware de teste antigo **não expunha nenhum desses endpoints**.

### 3. Boot incompatível com provisionamento

Antes desta atualização, o firmware ainda nascia com SSID STA hardcoded em `app_config.h`, o que ia contra o fluxo novo do produto. Em um equipamento novo, o comportamento correto é:

- subir AP local
- aguardar provisionamento
- só depois tentar entrar no Wi-Fi do estabelecimento

## Mudanças realizadas

### 1. Ajuste dos defaults de conectividade

Arquivo:

- `include/app_config.h`

Mudanças:

- remoção do SSID/senha STA hardcoded de fábrica
- inclusão de defaults para AP local:
  - prefixo do SSID do AP
  - porta do servidor local
  - CORS habilitado
  - limites de buffer HTTP
  - atraso para reinício após provisionamento

Com isso, um equipamento “limpo” passa a subir pronto para o fluxo de provisionamento do app.

### 2. Salvamento de provisionamento em NVS

Arquivos:

- `include/comm_storage.h`
- `src/comm_storage.c`

Mudança principal:

- criação de `comm_storage_save_provisioning(deviceId, wifiSsid, wifiPassword)`

Essa função salva no namespace `commcfg`:

- `wifi_en`
- `wifi_ssid`
- `wifi_pass`
- `device_id`

Isso preserva a separação entre:

- NVS antiga da IHM (`ihm`)
- NVS de comunicação (`commcfg`)

### 3. Wi-Fi com AP automático + STA quando configurado

Arquivos:

- `include/wifi_manager.h`
- `src/wifi_manager.c`

Mudanças:

- o firmware agora sobe AP local automaticamente
- quando houver rede configurada, sobe em `APSTA`
- quando não houver rede configurada, sobe em `AP`
- o AP é mantido como fallback local
- o SSID do AP passa a ser derivado do `deviceId`
- ao obter IP em STA, o firmware continua iniciando SNTP e MQTT

Resultado:

- o fluxo principal do app passa a funcionar
- o fallback local continua disponível quando a nuvem falhar

### 4. Servidor HTTP local para onboarding e contingência

Arquivos:

- `include/local_server.h`
- `src/local_server.c`
- `src/CMakeLists.txt`

Foi adicionada a camada HTTP local com CORS para funcionar também no app web.

Endpoints implementados:

- `GET /api/v1/ping`
- `GET /api/v1/status`
- `GET /api/v1/state`
- `GET /api/v1/capabilities`
- `GET /api/v1/diagnostics`
- `GET /api/v1/schedules`
- `POST /api/v1/commands`
- `POST /api/v1/schedules`
- `POST /api/v1/provisioning`

Também foram adicionados handlers `OPTIONS` para preflight.

### 5. Endpoint de provisionamento alinhado ao app

Endpoint novo:

- `POST /api/v1/provisioning`

Payload aceito:

```json
{
  "deviceId": "ihm32-AB18F0",
  "wifiSsid": "WiFi do estabelecimento",
  "wifiPassword": "senha-do-wifi"
}
```

Comportamento:

- valida `deviceId`
- valida `wifiSsid`
- salva os dados no NVS
- responde sucesso para o app
- agenda reinício automático da IHM

Esse reinício foi escolhido de forma proposital para reduzir risco de regressão no firmware legado de teste. Assim, a IHM volta já com:

- `deviceId` persistido
- Wi-Fi STA configurado
- AP local mantido
- tentativa limpa de conexão ao broker

### 6. Diagnóstico local compatível com o app

Arquivos:

- `include/protocol_json.h`
- `src/protocol_json.c`
- `src/local_server.c`

Foi adicionada a serialização de `diagnostics` em formato compatível com o app atualizado.

## Fluxo final após a atualização

### Fluxo principal com internet

1. IHM liga
2. AP local sobe automaticamente
3. usuário conecta o celular nesse AP
4. app envia `deviceId + wifiSsid + wifiPassword`
5. firmware salva em NVS
6. firmware reinicia
7. IHM sobe em `APSTA`
8. IHM tenta conectar na rede do estabelecimento
9. ao obter IP, inicia SNTP e MQTT
10. a operação normal passa a ocorrer pela cloud

### Fluxo fallback sem internet

Se a nuvem não estiver acessível:

- a IHM mantém o AP local
- o servidor local continua ativo
- o app pode usar o fallback HTTP local quando estiver fisicamente próximo

## Compatibilidade final por área

### Provisionamento

Status: **corrigido**

Compatível com o app atualizado:

- AP local automático
- endpoint `/api/v1/provisioning`
- persistência de `deviceId`, SSID e senha
- reboot para aplicar a nova conectividade

### Cloud MQTT

Status: **mantido compatível**

Continuam funcionais:

- publish de `status/state/capabilities`
- receive de `commands`
- receive de `schedules`
- publish de `events/errors`

### Fallback local

Status: **corrigido**

Agora compatível com o app para:

- teste de conexão local
- leitura de status/state/capabilities
- envio de comandos locais
- leitura e gravação de schedules
- diagnóstico local

### OTA

Status: **pendência opcional**

O app atualizado já reserva a área técnica para OTA, mas este firmware de teste ainda não expõe um endpoint OTA local nem fluxo OTA completo. Isso não bloqueia:

- onboarding
- cloud
- fallback local
- comandos
- schedules

Mas continua sendo uma melhoria futura recomendada.

## Mudanças obrigatórias x opcionais

### Mudanças obrigatórias

Estas eram necessárias para compatibilidade real com o app:

- remover dependência de SSID STA hardcoded
- subir AP local automaticamente
- implementar `/api/v1/provisioning`
- salvar provisionamento em NVS
- expor HTTP local para fallback

### Melhorias opcionais

Estas podem ser feitas depois:

- desligamento inteligente do AP quando cloud estiver estável
- endpoint local de OTA
- endpoints locais de `events` e `errors`
- discovery LAN automático
- políticas mais refinadas para provisionamento/reprovisionamento sem reboot

## Arquivos alterados

- `include/app_config.h`
- `include/comm_storage.h`
- `include/local_server.h`
- `include/log_tags.h`
- `include/protocol_json.h`
- `include/wifi_manager.h`
- `src/CMakeLists.txt`
- `src/comm_storage.c`
- `src/local_server.c`
- `src/mqtt_app.c`
- `src/protocol_json.c`
- `src/wifi_manager.c`

## Validação realizada

Foi validada a compilação com:

```bash
pio run
```

Resultado:

- build concluído com sucesso para `esp32dev`

## Pendências conhecidas

- OTA ainda não foi integrada neste firmware de teste
- não foi implementado endpoint HTTP local de `events/errors`; o app já tolera isso no fallback local
- não foi feita descoberta LAN automática

## Conclusão

O firmware de teste **não estava totalmente compatível** com o app atualizado por causa do novo fluxo de provisionamento e do fallback local.

Após esta atualização, ele passa a ficar compatível com o que hoje é essencial no aplicativo:

- provisionamento principal por AP local da IHM
- operação cloud via MQTT
- fallback local HTTP quando não houver internet/cloud

Tudo isso foi feito sem reescrever o firmware e sem mexer desnecessariamente na lógica principal já existente da IHM.
