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

#define DEFAULT_SERVER_IP   "127.0.0.1"
#define DEFAULT_SERVER_PORT 8080
#define CONFIG_FILE         "client.conf"
#define RETRY_INTERVAL_SEC  3

typedef struct { int server_fd; } ReceiverArgs;
void *receiver_thread(void *arg);
void *sender_thread(void *arg);

// ── ANSI ──
#define R      "\033[0m"
#define BOLD   "\033[1m"
#define CYAN   "\033[36m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define RED    "\033[31m"
#define GRAY   "\033[90m"

// ──────────────────────────────────────────
//  Чтение client.conf
// ──────────────────────────────────────────
static void load_config(char *ip_out, int ip_maxlen, int *port_out)
{
    strncpy(ip_out, DEFAULT_SERVER_IP, ip_maxlen - 1);
    *port_out = DEFAULT_SERVER_PORT;

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        f = fopen(CONFIG_FILE, "w");
        if (f) {
            fprintf(f,
                "# Адрес и порт сервера мессенджера\n"
                "server=%s\n"
                "port=%d\n",
                DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT);
            fclose(f);
        }
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = '\0';
        if      (strncmp(line, "server=", 7) == 0)
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
            fprintf(stderr, RED "Неверный IP: %s\n" R, ip);
            close(fd); return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;

        close(fd);
        attempt++;
        if (attempt == 1)
            printf(YELLOW "Сервер недоступен (%s:%d). Повтор каждые %d сек...\n" R,
                   ip, port, RETRY_INTERVAL_SEC);
        else
            printf("\r" GRAY "  Попытка %d..." R, attempt);
        fflush(stdout);
        sleep(RETRY_INTERVAL_SEC);
    }
}

// ──────────────────────────────────────────
//  Вспомогательные функции ввода
// ──────────────────────────────────────────
static int read_line_stdin(const char *prompt, char *buf, int maxlen)
{
    printf("%s%s%s", CYAN, prompt, R); fflush(stdout);
    if (!fgets(buf, maxlen, stdin)) return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return (int)strlen(buf);
}

static void read_password(const char *prompt, char *buf, int maxlen)
{
    printf("%s%s%s", CYAN, prompt, R); fflush(stdout);
    system("stty -echo");
    if (fgets(buf, maxlen, stdin))
        buf[strcspn(buf, "\n")] = '\0';
    system("stty echo");
    printf("\n");
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

// ──────────────────────────────────────────
//  Ветка входа
//  Возвращает 0 при успехе, -1 при ошибке
// ──────────────────────────────────────────
static int do_login(int fd, char *out_login)
{
    char resp[64] = {0};

    // Ждём LOGIN от сервера (после HELLO)
    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "LOGIN", 5) != 0) return -1;

    char login[32] = {0}, password[64] = {0};
    if (read_line_stdin("  Логин   : ", login, sizeof(login)) <= 0) return -1;

    char line[128];
    snprintf(line, sizeof(line), "%s\n", login);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "PASSWORD", 8) != 0) return -1;

    read_password("  Пароль  : ", password, sizeof(password));
    snprintf(line, sizeof(line), "%s\n", password);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "OK", 2) == 0) {
        strncpy(out_login, login, 31);
        return 0;
    }

    printf(RED "\n  Неверный логин или пароль.\n" R);
    return -1;
}

// ──────────────────────────────────────────
//  Ветка регистрации
//  Возвращает 0 при успехе, -1 при ошибке
// ──────────────────────────────────────────
static int do_register(int fd, char *out_login)
{
    char resp[64] = {0};

    // Ждём REG_LOGIN от сервера
    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;
    if (strncmp(resp, "REG_LOGIN", 9) != 0) return -1;

    printf(GRAY
           "\n  Логин: только латинские буквы, цифры и _ (минимум 2 символа)\n"
           "  Пароль: минимум 4 символа\n\n" R);

    char login[32] = {0}, password[64] = {0};
    if (read_line_stdin("  Логин   : ", login, sizeof(login)) <= 0) return -1;

    char line[128];
    snprintf(line, sizeof(line), "%s\n", login);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;

    // Обрабатываем ошибки логина
    if (strcmp(resp, "REG_FAIL_SHORT") == 0) {
        printf(RED "  Логин слишком короткий (минимум 2 символа)\n" R);
        return -1;
    }
    if (strcmp(resp, "REG_FAIL_CHARS") == 0) {
        printf(RED "  Логин содержит недопустимые символы\n" R);
        return -1;
    }
    if (strcmp(resp, "REG_FAIL_EXISTS") == 0) {
        printf(RED "  Пользователь с таким логином уже существует\n" R);
        return -1;
    }
    if (strncmp(resp, "REG_PASSWORD", 12) != 0) return -1;

    read_password("  Пароль  : ", password, sizeof(password));

    // Подтверждение пароля
    char password2[64] = {0};
    read_password("  Повтор  : ", password2, sizeof(password2));
    if (strcmp(password, password2) != 0) {
        // Отправляем серверу заведомо неверный пароль чтобы отменить
        send(fd, "\n", 1, 0);
        printf(RED "  Пароли не совпадают\n" R);
        return -1;
    }

    snprintf(line, sizeof(line), "%s\n", password);
    send(fd, line, strlen(line), 0);

    if (recv_line_fd(fd, resp, sizeof(resp)) < 0) return -1;

    if (strcmp(resp, "REG_FAIL_WEAK") == 0) {
        printf(RED "  Пароль слишком короткий (минимум 4 символа)\n" R);
        return -1;
    }
    if (strcmp(resp, "REG_OK") == 0) {
        strncpy(out_login, login, 31);
        printf(GREEN "\n  Аккаунт создан!\n" R);
        return 0;
    }

    printf(RED "  Ошибка регистрации\n" R);
    return -1;
}

// ──────────────────────────────────────────
//  Меню входа/регистрации
//  Возвращает заполненный out_login при успехе
// ──────────────────────────────────────────
static int auth_menu(const char *server_ip, int server_port, char *out_login)
{
    while (1) {
        printf(BOLD CYAN
               "\n╔══════════════════════════════════════╗\n"
               "║     МЕССЕНДЖЕР — ДОБРО ПОЖАЛОВАТЬ    ║\n"
               "╚══════════════════════════════════════╝\n" R
               GRAY "  Сервер: %s:%d\n\n" R
               "  " BOLD "1." R " Войти\n"
               "  " BOLD "2." R " Зарегистрироваться\n"
               "  " BOLD "0." R " Выход\n\n",
               server_ip, server_port);

        char choice[8] = {0};
        if (read_line_stdin("  Выбор: ", choice, sizeof(choice)) < 0) return -1;

        if (strcmp(choice, "0") == 0) return -1;

        if (strcmp(choice, "1") != 0 && strcmp(choice, "2") != 0) {
            printf(YELLOW "  Введите 1, 2 или 0\n" R);
            continue;
        }

        // Подключаемся к серверу
        int fd = connect_with_retry(server_ip, server_port);
        if (fd < 0) return -1;

        // Ждём HELLO от сервера
        char hello[16] = {0};
        if (recv_line_fd(fd, hello, sizeof(hello)) < 0 ||
            strncmp(hello, "HELLO", 5) != 0) {
            close(fd);
            printf(RED "  Ошибка протокола\n" R);
            continue;
        }

        if (strcmp(choice, "2") == 0) {
            // Регистрация
            printf("\n");
            send(fd, "REGISTER\n", 9, 0);
            int res = do_register(fd, out_login);
            if (res == 0) return fd; // успех — возвращаем открытый сокет
            close(fd);
        } else {
            // Вход
            printf("\n");
            send(fd, "LOGIN\n", 6, 0);
            int res = do_login(fd, out_login);
            if (res == 0) return fd; // успех
            close(fd);
        }

        // Ошибка — предлагаем попробовать снова
        printf(YELLOW "\n  Нажмите Enter чтобы попробовать снова...\n" R);
        getchar();
    }
}

// ──────────────────────────────────────────
//  main
// ──────────────────────────────────────────
int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    char server_ip[64] = {0};
    int  server_port   = DEFAULT_SERVER_PORT;
    load_config(server_ip, sizeof(server_ip), &server_port);

    char login[32] = {0};

    // Показываем меню входа/регистрации
    int server_fd = auth_menu(server_ip, server_port, login);
    if (server_fd < 0) {
        printf("До свидания!\n");
        return 0;
    }

    printf(GREEN BOLD "\n  Добро пожаловать, %s!\n\n" R, login);
    sleep(1);

    // Запускаем UI и receiver
    ui_init(login);

    static ReceiverArgs r_args;
    r_args.server_fd = server_fd;
    pthread_t receiver_tid;
    pthread_create(&receiver_tid, nullptr, receiver_thread, &r_args);

    ui_run(server_fd);

    ui_cleanup();
    shutdown(server_fd, SHUT_RDWR);
    pthread_join(receiver_tid, nullptr);
    close(server_fd);

    printf("До свидания!\n");
    return 0;
}