#include "auth.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

// ──────────────────────────────────────────
//  hash_password — SHA-256 → hex строка
// ──────────────────────────────────────────
const char *hash_password(const char *password)
{
    static char hex_out[65];
    unsigned char hash[SHA256_DIGEST_LENGTH];

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, password, strlen(password));
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        snprintf(hex_out + i * 2, 3, "%02x", hash[i]);
    hex_out[64] = '\0';
    return hex_out;
}

// ── Вспомогательная: читает строку из сокета до '\n' ──
static int recv_line(int fd, char *buf, int maxlen)
{
    int i = 0; char c;
    while (i < maxlen - 1) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

// ── Проверяет существование пользователя в users.dat ──
static int user_exists(const char *login)
{
    FILE *f = fopen(USERS_DAT, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char file_login[32] = {0};
        sscanf(line, "%31[^:]", file_login);
        if (strcmp(login, file_login) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

// ──────────────────────────────────────────
//  register_user
//
//  Протокол регистрации:
//    ← "REG_LOGIN\n"     сервер запрашивает логин
//    → "<логин>\n"
//    ← "REG_PASSWORD\n"  сервер запрашивает пароль
//    → "<пароль>\n"
//    ← "REG_OK\n"        успех
//    ← "REG_FAIL\n"      логин уже занят или ошибка
// ──────────────────────────────────────────
static int register_user(int client_fd, char *out_login)
{
    char login[32]    = {0};
    char password[64] = {0};

    send(client_fd, "REG_LOGIN\n", 10, 0);
    if (recv_line(client_fd, login, sizeof(login)) <= 0) return -1;

    // Проверяем что логин не пустой и состоит из допустимых символов
    if (strlen(login) < 2) {
        send(client_fd, "REG_FAIL_SHORT\n", 15, 0);
        return -1;
    }
    for (int i = 0; login[i]; i++) {
        char c = login[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            send(client_fd, "REG_FAIL_CHARS\n", 15, 0);
            return -1;
        }
    }

    // Проверяем что логин не занят
    if (user_exists(login)) {
        send(client_fd, "REG_FAIL_EXISTS\n", 16, 0);
        return -1;
    }

    send(client_fd, "REG_PASSWORD\n", 13, 0);
    if (recv_line(client_fd, password, sizeof(password)) <= 0) return -1;

    if (strlen(password) < 4) {
        send(client_fd, "REG_FAIL_WEAK\n", 14, 0);
        return -1;
    }

    // Записываем нового пользователя в users.dat
    const char *hash = hash_password(password);
    FILE *f = fopen(USERS_DAT, "a");
    if (!f) {
        log_event("ОШИБКА: не удалось открыть %s для записи", USERS_DAT);
        send(client_fd, "REG_FAIL\n", 9, 0);
        return -1;
    }
    fprintf(f, "%s:%s:%s\n", login, hash, login); // имя по умолчанию = логин
    fclose(f);

    send(client_fd, "REG_OK\n", 7, 0);
    strncpy(out_login, login, 31);
    log_event("Зарегистрирован новый пользователь: %s", login);
    return 0;
}

// ──────────────────────────────────────────
//  authenticate
//
//  Расширенный протокол:
//    ← "HELLO\n"         сервер приветствует
//    → "LOGIN\n"         клиент хочет войти
//       или
//    → "REGISTER\n"      клиент хочет зарегистрироваться
//
//  Ветка LOGIN:
//    ← "LOGIN\n"
//    → "<логин>\n"
//    ← "PASSWORD\n"
//    → "<пароль>\n"
//    ← "OK\n" / "FAIL\n"
//
//  Ветка REGISTER:
//    ← "REG_LOGIN\n"
//    → "<логин>\n"
//    ← "REG_PASSWORD\n"
//    → "<пароль>\n"
//    ← "REG_OK\n" / "REG_FAIL*\n"
//    После REG_OK клиент сразу входит под новым аккаунтом.
// ──────────────────────────────────────────
int authenticate(int client_fd, char *out_login)
{
    char intent[32] = {0};

    // Приветствие
    send(client_fd, "HELLO\n", 6, 0);
    if (recv_line(client_fd, intent, sizeof(intent)) <= 0) return -1;

    // Ветка регистрации
    if (strcmp(intent, "REGISTER") == 0) {
        int res = register_user(client_fd, out_login);
        if (res < 0) return -1;
        // После успешной регистрации сразу считаем пользователя вошедшим
        log_event("Пользователь '%s' вошёл после регистрации", out_login);
        return 0;
    }

    // Ветка входа (LOGIN или любое другое — стандартный флоу)
    char login[32]    = {0};
    char password[64] = {0};

    send(client_fd, "LOGIN\n", 6, 0);
    if (recv_line(client_fd, login, sizeof(login)) <= 0) return -1;

    send(client_fd, "PASSWORD\n", 9, 0);
    if (recv_line(client_fd, password, sizeof(password)) <= 0) return -1;

    const char *input_hash = hash_password(password);

    FILE *f = fopen(USERS_DAT, "r");
    if (!f) {
        log_event("ОШИБКА: не удалось открыть %s", USERS_DAT);
        send(client_fd, "FAIL\n", 5, 0);
        return -1;
    }

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char file_login[32] = {0}, file_hash[65] = {0}, file_name[64] = {0};
        if (sscanf(line, "%31[^:]:%64[^:]:%63s",
                   file_login, file_hash, file_name) < 2) continue;
        if (strcmp(login, file_login) == 0 &&
            strcmp(input_hash, file_hash) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);

    if (found) {
        send(client_fd, "OK\n", 3, 0);
        strncpy(out_login, login, 31);
        out_login[31] = '\0';
        log_event("Аутентификация успешна: %s", login);
        return 0;
    } else {
        send(client_fd, "FAIL\n", 5, 0);
        log_event("Ошибка аутентификации: %s", login);
        return -1;
    }
}