// subscriber.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#define DEVICE_PATH "/dev/pubsub"
#define BUFFER_SIZE 512

// Função auxiliar para enviar um comando ao driver.
void send_command(int fd, const char *command) {
    if (write(fd, command, strlen(command)) < 0) {
        perror("Erro ao enviar comando para o driver");
    }
}

int main(int argc, char *argv[]) {
    int fd;
    char buffer[BUFFER_SIZE];
    char command_buffer[BUFFER_SIZE];
    char *current_topic; // Ponteiro para armazenar o nome do tópico ativo
    ssize_t bytes_read;
    fd_set read_fds;

    // Validação dos Argumentos
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <topico_inicial>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    // Alocamos memória para o nome do tópico para poder alterá-lo depois.
    current_topic = strdup(argv[1]);
    if (!current_topic) {
        perror("Erro ao alocar memória para o nome do tópico");
        return EXIT_FAILURE;
    }

    // Abrindo o Dispositivo
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Erro ao abrir o dispositivo");
        free(current_topic);
        return EXIT_FAILURE;
    }

    // Etapa 1: Inscrever-se e Escutar o Tópico Inicial
    printf("Inscrevendo-se no tópico inicial '%s'...\n", current_topic);
    snprintf(command_buffer, BUFFER_SIZE, "/subscribe %s", current_topic);
    send_command(fd, command_buffer);

    printf("Escutando o tópico inicial '%s'...\n", current_topic);
    snprintf(command_buffer, BUFFER_SIZE, "/fetch %s", current_topic);
    send_command(fd, command_buffer);

    // Instruções para o Usuário
    printf("\n--- Inscrição bem-sucedida! ---\n");
    printf("Comandos disponíveis:\n");
    printf("  /subscribe <outro_topico>  - Inscreve-se em um novo tópico.\n");
    printf("  /listen <nome_do_topico>   - Troca o tópico ativo para leitura.\n");
    printf("Qualquer outro texto será publicado no tópico ativo ('%s').\n", current_topic);
    printf("Pressione Ctrl+C para sair.\n\n");

    // Loop Principal Interativo
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // Monitora o teclado
        FD_SET(fd, &read_fds);           // Monitora o dispositivo

        if (select(fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("Erro no select()");
            break;
        }

        // --- Checa se há atividade no teclado (entrada do usuário) ---
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE - 1);
            if (bytes_read > 0) {
                buffer[bytes_read - 1] = '\0'; // Remove o '\n'

                // --- Lógica de Comandos ---
                if (strncmp(buffer, "/subscribe ", 11) == 0) {
                    char *new_topic = buffer + 11;
                    snprintf(command_buffer, BUFFER_SIZE, "/subscribe %s", new_topic);
                    send_command(fd, command_buffer);
                    printf(">>> Inscrito com sucesso no novo tópico: '%s'\n", new_topic);

                } else if (strncmp(buffer, "/listen ", 8) == 0) {
                    char *topic_to_listen = buffer + 8;
                    snprintf(command_buffer, BUFFER_SIZE, "/fetch %s", topic_to_listen);
                    send_command(fd, command_buffer);

                    // Atualiza o nome do tópico ativo
                    free(current_topic);
                    current_topic = strdup(topic_to_listen);

                    printf(">>> Agora escutando o tópico: '%s'\n", current_topic);

                } else {
                    // Se não for um comando, é uma mensagem para publicar
                    snprintf(command_buffer, BUFFER_SIZE, "/publish %s \"%s\"", current_topic, buffer);
                    send_command(fd, command_buffer);
                    printf(">>> [Publicado em '%s']: %s\n", current_topic, buffer);
                }
            }
        }

        // --- Checa se há atividade no dispositivo (nova mensagem recebida) ---
        if (FD_ISSET(fd, &read_fds)) {
            bytes_read = read(fd, buffer, BUFFER_SIZE - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                // A mensagem vem do tópico que estamos escutando ativamente
                printf("<<< [Mensagem de '%s']: %s\n", current_topic, buffer);
            } else if (bytes_read < 0) {
                perror("Erro ao ler do dispositivo");
                break;
            }
        }
    }

    close(fd);
    free(current_topic); // Libera a memória alocada para o nome do tópico
    printf("\nDispositivo fechado. Encerrando.\n");
    return EXIT_SUCCESS;
}
