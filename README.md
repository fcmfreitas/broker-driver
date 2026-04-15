# Linux Kernel Broker Driver (QEMU)

Este repositório contém um módulo carregável do kernel (LKM) que implementa um sistema simples de mensageria (Broker) seguindo o modelo Publicador/Assinante (Pub/Sub). O projeto foi desenhado para ser compilado e testado dentro de um ambiente emulado com **QEMU** e **Buildroot**.

## 🛠️ Funcionalidades

- **Criação Dinâmica de Tópicos**: Gerenciamento de tópicos de comunicação através de chamadas ao driver.
- **Modelo Pub/Sub**: Suporte a múltiplos assinantes e publicadores no espaço de usuário.
- **Sincronização no Kernel**: Implementação de mecanismos para garantir a integridade das mensagens entre processos.
- **Interface via Device Files**: Comunicação simplificada através de arquivos de dispositivo em `/dev/`.

## 📂 Estrutura do Projeto

- `pubsub_driver.c`: O código principal do módulo do kernel Linux.
- `topic_creator.c`: Utilitário para gerenciar a criação e remoção de tópicos.
- `subscriber.c`: Aplicação de exemplo para consumir mensagens de um tópico.
- `Makefile`: Script para compilação cruzada (Cross-compilation) do módulo.

## 🚀 Como Compilar e Rodar no QEMU

1. **Configurar o ambiente**:
   Certifique-se de ter as fontes do kernel Linux (ou os headers) configuradas para o seu alvo (ex: x86_64 ou ARM).

2. **Compilar o módulo**:
   ```bash
   make
