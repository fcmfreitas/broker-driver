// subscriber.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/pubsub"
#define BUFFER_SIZE 512

int main(int argc, char *argv[]) {
    int fd;
    char buffer[BUFFER_SIZE];

    if (argc != 2) {
        fprintf(stderr, "Uso: %s <topico>\n", argv[0]);
        return 1;
    }

    char *topic = argv[1];

    printf("Inscrito: Abrindo dispositivo %s...\n", DEVICE_PATH);
    // Abre para leitura e escrita
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Falha ao abrir o dispositivo");
        return 1;
    }

    // 1. Enviar comando de inscrição
    snprintf(buffer, BUFFER_SIZE, "/subscribe %s", topic);
    printf("Inscrito: Enviando comando: %s\n", buffer);
    if (write(fd, buffer, strlen(buffer)) < 0) {
        perror("Falha ao se inscrever no topico");
        close(fd);
        return 1;
    }
    printf("Inscrito: Inscricao enviada com sucesso.\n");

    // 2. Enviar comando para preparar a leitura
    snprintf(buffer, BUFFER_SIZE, "/fetch %s", topic);
     printf("Inscrito: Enviando comando: %s\n", buffer);
    if (write(fd, buffer, strlen(buffer)) < 0) {
        perror("Falha ao dar fetch no topico");
        close(fd);
        return 1;
    }
    printf("Inscrito: Fetch enviado com sucesso. Aguardando mensagens...\n");

    // 3. Loop para ler mensagens
    while (1) {
        ssize_t bytes_read = read(fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read < 0) {
            perror("Falha ao ler do dispositivo");
            break;
        }
        if (bytes_read == 0) {
            // Nenhuma mensagem nova, aguarda um pouco
            sleep(2);
            continue;
        }

        buffer[bytes_read] = '\0';
        printf(">>> Mensagem recebida: %s\n", buffer);
    }

    close(fd);
    return 0;
}
