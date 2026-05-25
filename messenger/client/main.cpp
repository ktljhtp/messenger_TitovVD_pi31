// ══════════════════════════════════════════════════════════
//  messenger_client — точка входа клиента
//
//  Конфиг: client.conf (рядом с бинарником)
//  Формат: server=192.168.1.10
//          port=8080
//
//  Если конфига нет — создаёт с дефолтными значениями.
//  При недоступности сервера — повторяет попытки каждые 3 сек.
// ══════════════════════════════════════════════════════════
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
#include <signal.h>

// ── Настройки по умолчанию ──
#define DEFAULT_SERVER_IP   "127.0.0.1"
#define DEFAULT_SERVER_PORT 8080
#define CONFIG_FILE         "client.conf"
#define RETRY_INTERVAL_SEC  3
#define MAX_RETRIES         0   // 0 = бесконечно

typedef struct { int server_fd; } ReceiverArgs;
void *receiver_thread(void *arg);
void *sender_thread(void *arg);

// ──────────────────────────────────────────
//  Чтение / создание client.conf
// ──────────────────────────────────────────
static void load_config(char *ip_out, int ip_maxlen, int *port_out)
{
    // Дефолты
    strncpy(ip_out, DEFAULT_SERVER_IP, ip_maxlen - 1);
    *port_out = DEFAULT_SERVER_PORT;

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        // Создаём конфиг с дефолтными значениями
        f = fopen(CONFIG_FILE, "w");
        if (f) {
            fprintf(f,
                "# Адрес и порт сервера мессенджера\n"
                "server=%s\n"
                "port=%d\n",
                DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT);
            fclose(f);
            printf("Создан конфиг: %s\n"
                   "Отредактируй его чтобы указать адрес сервера.\n\n",
                   CONFIG_FILE);
        }
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "server=", 7) == 0)
            strncpy(ip_out, line + 7, ip_maxlen - 1);
        else if (strncmp(line, "port=", 5) == 0)
            *port_out = atoi(line + 5);
    }
    fclose(f);
}

// ──────────────────────────────────────────
//  Подключение с автоповтором
// ──────────────────────────────────────────
static int connect_with_retry(const char *ip, int port)
{
    int attempt = 0;
    while (1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)port);

        if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
            fprintf(stderr, "Неверный IP в конфиге: %s\n", ip);
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd; // успех

        close(fd);
        attempt++;

        if (attempt == 1)
            printf("Сервер недоступен (%s:%d). Повтор каждые %d сек...\n",
                   ip, port, RETRY_INTERVAL_SEC);
        else
            printf("\r  Попытка %d...", attempt);
        fflush(stdout);

        sleep(RETRY_INTERVAL_SEC);
    }
}

// ──────────────────────────────────────────
//  Чтение строки из сокета до '\n'
// ──────────────────────────────────────────
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

// ──────────────────────────────────────────
//  Аутентификация
// ──────────────────────────────────────────
static int do_auth(int fd, const char *login, const char *password)
{
    char resp[64];

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;

    if (strncmp(resp, "BUSY", 4) == 0) {
        printf("\nСервер занят (достигнут лимит подключений).\n"
               "Попробуйте позже.\n");
        return -1;
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

    printf("\nНеверный логин или пароль.\n");
    return -1;
}

// ──────────────────────────────────────────
//  Ввод пароля без эха
// ──────────────────────────────────────────
static void read_password(const char *prompt, char *buf, int maxlen)
{
    printf("%s", prompt); fflush(stdout);
    system("stty -echo");
    if (fgets(buf, maxlen, stdin))
        buf[strcspn(buf, "\n")] = '\0';
    system("stty echo");
    printf("\n");
}

// ──────────────────────────────────────────
//  main
// ──────────────────────────────────────────
int main(void)
{
    signal(SIGPIPE, SIG_IGN); // игнорируем разрыв соединения при записи

    // ── Читаем конфиг ──
    char server_ip[64] = {0};
    int  server_port   = DEFAULT_SERVER_PORT;
    load_config(server_ip, sizeof(server_ip), &server_port);

    printf("╔══════════════════════════════════════╗\n"
           "║        МЕССЕНДЖЕР — КЛИЕНТ           ║\n"
           "╚══════════════════════════════════════╝\n"
           "  Сервер: %s:%d\n\n", server_ip, server_port);

    // ── Ввод логина и пароля ──
    char login[32]    = {0};
    char password[64] = {0};

    printf("Логин   : "); fflush(stdout);
    if (!fgets(login, sizeof(login), stdin)) return 1;
    login[strcspn(login, "\n")] = '\0';

    read_password("Пароль  : ", password, sizeof(password));

    // ── Подключение с автоповтором ──
    printf("\nПодключение к %s:%d...\n", server_ip, server_port);
    int server_fd = connect_with_retry(server_ip, server_port);
    if (server_fd < 0) return 1;
    printf("\rПодключено!                    \n");

    // ── Аутентификация (повтор при неудаче) ──
    while (do_auth(server_fd, login, password) < 0) {
        close(server_fd);

        printf("Повторить вход? (Enter — да, Ctrl+C — выход): ");
        fflush(stdout);
        char ans[4] = {0};
        if (!fgets(ans, sizeof(ans), stdin)) return 1;

        // Перезапрашиваем логин и пароль
        printf("Логин   : "); fflush(stdout);
        if (!fgets(login, sizeof(login), stdin)) return 1;
        login[strcspn(login, "\n")] = '\0';
        read_password("Пароль  : ", password, sizeof(password));

        // Переподключаемся
        server_fd = connect_with_retry(server_ip, server_port);
        if (server_fd < 0) return 1;
    }

    printf("Вход выполнен!\n\n");

    // ── Запуск UI ──
    ui_init(login);

    static ReceiverArgs r_args;
    r_args.server_fd = server_fd;
    pthread_t receiver_tid;
    pthread_create(&receiver_tid, nullptr, receiver_thread, &r_args);

    ui_run(server_fd);

    // ── Завершение ──
    ui_cleanup();
    shutdown(server_fd, SHUT_RDWR);
    pthread_join(receiver_tid, nullptr);
    close(server_fd);

    printf("До свидания!\n");
    return 0;
}