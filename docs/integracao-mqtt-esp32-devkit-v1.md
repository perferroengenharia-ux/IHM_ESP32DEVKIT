# Integração MQTT no firmware ESP32 DEVKIT V1

Este documento descreve a camada MQTT adicionada ao projeto `IHM_final-main`, preservando a lógica original concentrada em `src/main.c`.

## Estratégia adotada

A integração foi feita de forma pouco invasiva. A lógica antiga da IHM continua sendo a fonte de verdade para display, RS485, parâmetros, estados locais, LEDs, console serial e temporizações. A camada MQTT foi adicionada em módulos separados e conversa com o firmware antigo por um adapter implementado no próprio `main.c`, porque os estados e funções originais são `static`.

## Placa e build

O projeto permanece em PlatformIO com ESP-IDF e board `esp32dev`, compatível com ESP32 DEVKIT V1 / ESP32-WROOM-32.

Arquivos principais:

- `platformio.ini`
- `src/CMakeLists.txt`
- `sdkconfig.esp32dev`
- `partitions.csv`: particao customizada com app de 2 MB para dar folga ao firmware com MQTT

## Arquitetura adicionada

- `include/app_config.h`: defaults de Wi-Fi, MQTT, deviceId, topicPrefix e intervalos.
- `include/certs.h`: placeholder do certificado CA para broker MQTT TLS.
- `src/comm_storage.c`: leitura de configurações de comunicação em NVS, sem interferir na NVS antiga `ihm`.
- `src/wifi_manager.c`: inicialização Wi-Fi STA e reconexão básica.
- `src/mqtt_manager.c`: conexão MQTT, subscribe em comandos e publicação de estado/capacidades.
- `src/protocol_topics.c`: montagem centralizada dos tópicos MQTT.
- `src/protocol_json.c`: parsing de comandos e serialização de status, state, capabilities, eventos e erros.
- `src/mqtt_app.c`: bootstrap da camada de comunicação e task periódica de publicação.
- `include/ihm_mqtt_adapter.h`: interface entre comunicação e lógica antiga da IHM.

## Tópicos MQTT

O padrão é:

- `{topicPrefix}/{deviceId}/status`
- `{topicPrefix}/{deviceId}/state`
- `{topicPrefix}/{deviceId}/capabilities`
- `{topicPrefix}/{deviceId}/commands`
- `{topicPrefix}/{deviceId}/events`
- `{topicPrefix}/{deviceId}/errors`
- `{topicPrefix}/{deviceId}/schedules`

A montagem fica centralizada em `src/protocol_topics.c`.

## Como configurar MQTT

Edite `include/app_config.h` para valores padrão de teste:

- `APP_WIFI_STA_SSID_DEFAULT`
- `APP_WIFI_STA_PASSWORD_DEFAULT`
- `APP_MQTT_URI_DEFAULT`
- `APP_MQTT_PORT_DEFAULT`
- `APP_MQTT_USERNAME_DEFAULT`
- `APP_MQTT_PASSWORD_DEFAULT`
- `APP_TOPIC_PREFIX_DEFAULT`
- `APP_MQTT_ENABLED_DEFAULT`

O `deviceId` padrão é gerado pelo MAC do ESP32 no formato `ihm32-XXXXXX`. Futuramente ele pode ser sobrescrito pela NVS no namespace `commcfg`, chave `device_id`.

## Certificado CA

Para usar `mqtts://`, coloque o certificado PEM real em `include/certs.h` e habilite TLS em `APP_MQTT_USE_TLS_DEFAULT` ou via NVS.

## Comandos suportados

A camada MQTT aceita:

- `power-on`
- `power-off`
- `set-frequency` com `payload.freqTargetHz`
- `set-pump` com `payload.enabled`
- `set-swing` com `payload.enabled`
- `run-drain`
- `stop-drain`
- `request-status`
- `request-capabilities`

Frequência fora de `P20` e `P21` é rejeitada. Bomba, swing e dreno obedecem os parâmetros existentes `P82`, `P85`, `P81` e `P80`.

## Estados e capacidades publicados

O snapshot publicado para o app vem dos estados reais do firmware antigo:

- `system_on`, `motor_running`, `output_frequency`, `target_frequency`
- `bomba_on`, `swing_on`, `dreno_status`
- `water_shortage`, `current_state`, `current_error_code`
- parâmetros `P12`, `P20`, `P21`, `P30`, `P31`, `P32`, `P44`, `P80`, `P82`, `P83`, `P84`, `P85`

## Schedules e OTA

O tópico de `schedules` existe no contrato, mas a execução local de agendamentos ainda não foi integrada neste firmware. OTA remoto também ficou como evolução futura; o certificado CA já foi preparado para reaproveitamento.

## Como testar

1. Configure SSID/senha e broker em `include/app_config.h`.
2. Compile com `pio run`.
3. Grave na placa com `pio run -t upload`.
4. Monitore com `pio device monitor`.
5. Confira logs `wifi_manager` e `mqtt_manager`.
6. Publique comandos no tópico `{topicPrefix}/{deviceId}/commands`.
7. Observe respostas nos tópicos `state`, `status`, `events` e `errors`.

## O que foi preservado

A integração não substitui a lógica antiga da IHM. Foram preservados display multiplexado, LEDs, console serial, parâmetros, NVS antiga, RS485/UART com MI, watchdog de comunicação, tratamento de falhas e ciclos de pré-molhamento, secagem, dreno e exaustão.

## Cuidados

O firmware original é monolítico e usa estados globais `static`. Por isso o adapter foi colocado no próprio `main.c` para evitar expor todos os globais ou refatorar o projeto inteiro. Essa escolha preserva o comportamento, mas uma modularização futura pode mover a lógica da IHM para headers e fontes próprios.


