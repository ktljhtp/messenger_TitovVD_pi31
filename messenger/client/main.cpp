#include "ui.h"
#include "../common/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

typedef struct { int server_fd; } ReceiverArgs;
void *receiver_thread(void *arg);
void *sender_thread(void *arg);

static int connect_to_server(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Неверный IP: %s\n", ip); close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

static int recv_line_fd(int fd, char *buf, int maxlen)
{
    int i = 0; char c;
    while (i < maxlen - 1) {
        if (recv(fd, &c, 1, 0) <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static int do_auth(int fd, const char *login, const char *password)
{
    char resp[64];
    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "BUSY", 4) == 0) {
        fprintf(stderr, "Сервер занят, попробуйте позже\n"); return -1;
    }
    if (strncmp(resp, "LOGIN", 5) != 0) return -1;

    char line[64];
    snprintf(line, sizeof(line), "%s\n", login);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "PASSWORD", 8) != 0) return -1;

    snprintf(line, sizeof(line), "%s\n", password);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "OK", 2) == 0) return 0;

    fprintf(stderr, "Неверный логин или пароль\n");
    return -1;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <IP> <порт>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[2]);
    char login[32] = {0}, password[64] = {0};

    printf("Подключение к %s:%d\n", argv[1], port);
    printf("Логин   : "); fflush(stdout);
    if (!fgets(login, sizeof(login), stdin)) return 1;
    login[strcspn(login, "\n")] = '\0';

    printf("Пароль  : "); fflush(stdout);
    system("stty -echo");
    if (!fgets(password, sizeof(password), stdin)) { system("stty echo"); return 1; }
    system("stty echo");
    printf("\n");
    password[strcspn(password, "\n")] = '\0';

    int server_fd = connect_to_server(argv[1], port);
    if (server_fd < 0) return 1;

    if (do_auth(server_fd, login, password) < 0) {
        close(server_fd); return 1;
    }

    printf("Успешный вход!\n");

    // Инициализируем UI с логином
    ui_init(login);

    // Запускаем receiver в отдельном потоке
    static ReceiverArgs r_args;
    r_args.server_fd = server_fd;
    pthread_t receiver_tid;
    pthread_create(&receiver_tid, nullptr, receiver_thread, &r_args);

    // Главный цикл UI (блокирует до /quit)
    ui_run(server_fd);

    // Завершение
    ui_cleanup();
    shutdown(server_fd, SHUT_RDWR);
    pthread_join(receiver_tid, nullptr);
    close(server_fd);
    printf("До свидания!\n");
    return 0;
}