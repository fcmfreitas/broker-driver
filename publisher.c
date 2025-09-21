// publisher.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/pubsub"
#define BUFFER_SIZE 512

int main(int argc, char *argv[]) {
    int fd;
    char write_buf[BUFFER_SIZE];

    if (argc != 3) {
        fprintf(stderr, "Uso: %s <topico> \"<mensagem>\"\n", argv[0]);
        return 1;
    }

    char *topic = argv[1];
    char *message = argv[2];

    printf("Publicador: Abrindo dispositivo %s...\n", DEVICE_PATH);
    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Falha ao abrir o dispositivo");
        return 1;
    }

    snprintf(write_buf, BUFFER_SIZE, "/publish %s \"%s\"", topic, message);
    printf("Publicador: Enviando comando: %s\n", write_buf);

    ssize_t bytes_written = write(fd, write_buf, strlen(write_buf));
    if (bytes_written < 0) {
        perror("Falha ao escrever no dispositivo");
        close(fd);
        return 1;
    }

    printf("Publicador: Comando enviado com sucesso (%ld bytes).\n", bytes_written);
    close(fd);
    return 0;
}
