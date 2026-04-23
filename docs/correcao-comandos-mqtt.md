# Correcao da cadeia de comandos MQTT

## Causa raiz

A telemetria do ESP para o app ja estava funcional, mas o caminho inverso tinha dois problemas reais:

1. O app publicava comandos MQTT por conexoes WebSocket curtas e encerrava com `end(true)` logo depois do callback de publish.
   No navegador isso podia fechar a sessao antes da entrega efetiva do comando ao broker.
2. O retorno de comando do firmware era publicado em `events/errors`, mas o formato nao batia com o contrato consumido pelo app.
   Resultado: mesmo quando havia retorno, o app nao conseguia interpretar o ack corretamente.

## O que foi alterado

### App

- `C:\Users\murilo\axon-control\src\services\transports\mqtt-websocket-transport.ts`
- `C:\Users\murilo\axon-control\src\services\device-service.ts`

### Firmware

- `C:\Users\murilo\Downloads\IHM_final-main\src\mqtt_manager.c`
- `C:\Users\murilo\Downloads\IHM_final-main\src\protocol_json.c`

## Como ficou o fluxo de comandos

1. O app abre e mantem uma sessao MQTT WebSocket longa para o dispositivo ativo.
2. Quando o usuario envia um comando, o app publica no topico:

```text
axon/ihm/{deviceId}/commands
```

3. A publicacao agora usa QoS 1.
4. Se existir sessao ativa, o app reutiliza a conexao ja conectada.
5. O firmware assina o topico de comando ao conectar no broker e reexecuta o subscribe a cada reconexao.
6. O firmware recebe o payload, faz parse, valida `deviceId`, encaminha para `ihm_mqtt_adapter_execute_command()` e aplica a logica real da IHM.
7. Depois disso o firmware publica:
   - ack de sucesso em `axon/ihm/{deviceId}/events`
   - ack de falha em `axon/ihm/{deviceId}/errors`
   - `state/status` atualizado logo em seguida

## Topico de comando usado

```text
{topicPrefix}/{deviceId}/commands
```

Com o padrao atual:

```text
axon/ihm/{deviceId}/commands
```

## Payload esperado

Exemplo de `power-on`:

```json
{
  "schema": "axon.ihm.v1",
  "deviceId": "ihm32-AB18F0",
  "timestamp": "2026-04-23T12:00:00Z",
  "source": "app",
  "id": "cmd-123456",
  "type": "power-on",
  "payload": {}
}
```

Exemplo de `set-frequency`:

```json
{
  "schema": "axon.ihm.v1",
  "deviceId": "ihm32-AB18F0",
  "timestamp": "2026-04-23T12:00:05Z",
  "source": "app",
  "id": "cmd-123457",
  "type": "set-frequency",
  "payload": {
    "freqTargetHz": 35
  }
}
```

## Como o firmware aplica a logica real da IHM

O firmware nao usa um "estado MQTT" separado para fingir o comando.

A aplicacao real acontece em:

`C:\Users\murilo\Downloads\IHM_final-main\src\main.c`

Ponto principal:

- `ihm_mqtt_adapter_execute_command(...)`

Esse handler reaproveita a logica existente da IHM:

- `power-on` e `power-off` usam `dispatch_button_event(BTN_ONOFF, ...)`
- `set-frequency` altera `target_frequency` real, respeitando `P20/P21`
- `set-pump` usa o fluxo real de `BTN_CLIMATIZAR` / `BTN_VENTILAR`
- `set-swing` usa `dispatch_button_event(BTN_SWING, false)`
- `run-drain` / `stop-drain` usam `dispatch_button_event(BTN_DRENO, false)`

Ou seja, a ponte MQTT ficou ligada ao comportamento funcional ja existente da IHM.

## Ajustes de protocolo feitos

### Ack de comando

O ack MQTT agora segue o contrato esperado pelo app, incluindo erro estruturado completo quando houver falha:

- `id`
- `type`
- `accepted`
- `applied`
- `status`
- `state`
- `error`

### Erro de payload invalido

O firmware passou a publicar erro no formato de `errors[]`, compatível com o parser do app.

## Logs adicionados no firmware

Agora o serial mostra melhor o caminho do comando:

- subscribe realizado
- topico recebido
- payload recebido
- comando reconhecido
- `deviceId` divergente
- resultado do comando com `accepted/applied/status/code`
- publicacao de ack

## Como testar rapidamente

1. Grave o firmware atualizado no ESP.
2. Abra o monitor serial.
3. Confirme logs parecidos com:

```text
MQTT conectado
Subscribe realizado: commands=axon/ihm/{deviceId}/commands ...
```

4. No app, teste:

### Power on/off

- Toque em `Ligar`
- Verifique no serial o comando recebido
- Verifique o novo `state/status`
- Toque em `Desligar`

### Mudanca de frequencia

- Escolha um valor entre `fMinHz` e `fMaxHz`
- Toque em `Aplicar frequencia`
- Verifique no serial o `type=set-frequency`
- Confirme atualizacao de `target_frequency`

### Bomba

- Toque em `Ligar bomba` ou `Desligar bomba`
- Confirme no serial se o comando foi aceito ou bloqueado por regra valida da IHM

### Swing

- Toque em `Ligar swing` ou `Desligar swing`
- Confirme no serial o motivo caso o firmware rejeite por motor parado, pre-wet ou dry-run

## Resultado esperado depois da correcao

- o app continua recebendo telemetria
- os comandos sao entregues ao broker com confiabilidade maior
- o firmware recebe e interpreta os comandos
- o handler real da IHM e acionado
- o ack volta em formato coerente
- o novo estado e publicado de volta para o app
