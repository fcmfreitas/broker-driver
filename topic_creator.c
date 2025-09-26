// topic_creator.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/pubsub"
#define MAX_COMMAND_LEN 256

int main(int argc, char *argv[]) {
    int fd;
    char command_buffer[MAX_COMMAND_LEN];

    // O programa espera 2 argumentos: ./topic_creator <nome_do_topico>
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <nome_do_topico>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *topic = argv[1];

    printf("Abrindo o dispositivo %s...\n", DEVICE_PATH);
    fd = open(DEVICE_PATH, O_WRONLY); // Apenas escrita é necessária
    if (fd < 0) {
        perror("Erro ao abrir o dispositivo. O módulo pubsub_driver está carregado?");
        return EXIT_FAILURE;
    }

    // comando /subscribe
    // Este comando garante que o tópico seja criado na memória do kernel se ainda não existir.
    // Ele também cria uma inscrição temporária para o PID deste processo.
    snprintf(command_buffer, MAX_COMMAND_LEN, "/subscribe %s", topic);
    printf("Enviando comando para criar/garantir existência do tópico: '%s'\n", command_buffer);

    if (write(fd, command_buffer, strlen(command_buffer)) < 0) {
        perror("Erro ao enviar o comando de inscrição");
        close(fd);
        return EXIT_FAILURE;
    }

    // comando /unsubscribe
    // Remove imediatamente a inscrição que acabamos de criar.
    // Isso evita deixar uma inscrição "fantasma" no kernel. O tópico em permanece.
    snprintf(command_buffer, MAX_COMMAND_LEN, "/unsubscribe %s", topic);
    printf("Enviando comando para limpar a inscrição temporária: '%s'\n", command_buffer);

    if (write(fd, command_buffer, strlen(command_buffer)) < 0) {
        perror("Erro ao enviar o comando de cancelamento de inscrição");
        close(fd);
        return EXIT_FAILURE;
    }
    
    printf("\nOperação concluída com sucesso. Tópico '%s' está pronto para uso.\n", topic);

    //Fechando o Dispositivo
    close(fd);

    return EXIT_SUCCESS;
}
