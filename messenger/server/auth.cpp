#include "auth.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

// ──────────────────────────────────────────
//  hash_password
//
//  Возвращает SHA-256 хеш строки password
//  в виде hex-строки длиной 64 символа.
//
//  Использует статический буфер —
//  результат действителен до следующего вызова.
// ──────────────────────────────────────────
const char *hash_password(const char *password)
{
    static char hex_out[65]; // 32 байта * 2 символа + '\0'

    unsigned char hash[SHA256_DIGEST_LENGTH];

    // EVP-интерфейс (OpenSSL 3.x совместим)
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, password, strlen(password));
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    // Конвертируем байты в hex
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", hash[i]);
    }
    hex_out[64] = '\0';

    return hex_out;
}

// ──────────────────────────────────────────
//  recv_line — читает из сокета строку до '\n'
//  (вспомогательная функция, не экспортируется)
// ──────────────────────────────────────────
static int recv_line(int fd, char *buf, int maxlen)
{
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1; // разрыв соединения
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

// ──────────────────────────────────────────
//  authenticate
//
//  Протокол (текстовый, по одной строке):
//    ← "LOGIN\n"       (сервер запрашивает логин)
//    → "<логин>\n"     (клиент отвечает)
//    ← "PASSWORD\n"    (сервер запрашивает пароль)
//    → "<пароль>\n"    (клиент отвечает)
//    ← "OK\n"          (успех) / "FAIL\n" (ошибка)
//
//  Ищет совпадение в users.dat:
//    логин:sha256_хеш:имя
// ──────────────────────────────────────────
int authenticate(int client_fd, char *out_login)
{
    char login[32]    = {0};
    char password[64] = {0};

    // Запрашиваем логин
    send(client_fd, "LOGIN\n", 6, 0);
    if (recv_line(client_fd, login, sizeof(login)) <= 0) {
        return -1;
    }

    // Запрашиваем пароль
    send(client_fd, "PASSWORD\n", 9, 0);
    if (recv_line(client_fd, password, sizeof(password)) <= 0) {
        return -1;
    }

    // Хешируем введённый пароль
    const char *input_hash = hash_password(password);

    // Ищем пользователя в users.dat
    FILE *f = fopen(USERS_DAT, "r");
    if (!f) {
        log_event("ОШИБКА: не удалось открыть %s", USERS_DAT);
        send(client_fd, "FAIL\n", 5, 0);
        return -1;
    }

    char line[256];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        // Убираем \n в конце строки
        line[strcspn(line, "\n")] = '\0';

        // Парсим: логин:хеш:имя
        char file_login[32]  = {0};
        char file_hash[65]   = {0};
        char file_name[64]   = {0};

        if (sscanf(line, "%31[^:]:%64[^:]:%63s",
                   file_login, file_hash, file_name) < 2) {
            continue; // пропускаем битые строки
        }

        if (strcmp(login, file_login) == 0 &&
            strcmp(input_hash, file_hash) == 0)
        {
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
        log_event("Ошибка аутентификации для логина: %s", login);
        return -1;
    }
}
