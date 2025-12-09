# Projeto-final-micro---ENG4033
# Sistema de Controle de Presença por RFID

## Descrição do projeto

Este projeto implementa um **sistema de controle de presença** utilizando **Arduino + leitor RFID** e uma aplicação de servidor/interface.

**Objetivo geral:**
- Registrar a presença de alunos/usuários por meio de cartões RFID.
- Exibir e gerenciar as presenças em uma interface (web ou desktop).
- Permitir a exportação/integração dos dados para um backend (ex.: banco de dados MongoDB).

**Principais recursos:**
- Leitura de cartões RFID (MFRC522) conectados ao Arduino/ESP.
- Registro de horário de chegada.
- Armazenamento em memória/EEPROM e envio por **comunicação serial**.
- Interface de visualização dos dados (servidor Python/Flask).
- Possibilidade de sincronizar as presenças com um backend/banco de dados.

---

## Arquitetura geral

A arquitetura do sistema pode ser dividida em três partes principais:

### 1. Dispositivo físico (Arduino + sensores)

- Placa: **Arduino Mega 2560** .
- Módulo RFID: MFRC522 para leitura dos cartões.
- Display TFT para exibir mensagens ao usuário.
- RTC (relógio de tempo real) para obter data e hora. 
- Outros componentes: buzzer, botões(JKS Button, tela touch).

**Responsabilidades:**
- Ler o UID do cartão RFID.
- Verificar se o cartão é válido (pode ser consultando uma lista local, EEPROM ou tabela).
- Registrar horário de presença (hora/minuto, eventualmente dia).
- Enviar os registros de presença pela serial em um formato pré-definido (ex.: `PR_ALL:UID,hora,minuto|...`).

### 2. Comunicação serial

- A comunicação entre o Arduino e o servidor é feita via **porta serial**.
- O Arduino envia as presenças em formato de linha de texto, por exemplo:

  ```text
  PR_ALL:17287C34,15,30|37B37B34,16,10|

### 3. Fluxo no servidor
  - Implementado em Python(Flask).
  - Sincroniza e envia os alunos e horários para o arduino.
  - Lê continuamente a porta serial e recebe as presenças.
  - Interface web para visualização dos dados.

## *Montagem do Circuito no Fritzing*
A imagem abaixo mostra a conexão entre o Arduino Mega 2560 e os principais componentes do sistema:
![Conexão dos componentes no Arduino](docs/conexao_fritzing.png)


## *Video do projeto*

