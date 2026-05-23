#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <stdio.h>
#include <string.h>

// ──────────────────────────────────────────
//  Вспомогательная функция: печатает ошибку OpenSSL в stderr
// ──────────────────────────────────────────
static void print_openssl_error(const char *where)
{
    unsigned long err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    fprintf(stderr, "[crypto] %s: %s\n", where, buf);
}

// ──────────────────────────────────────────
//  encrypt_message
//
//  AES-256-CBC через EVP_EncryptUpdate + EVP_EncryptFinal_ex.
//  OpenSSL сам добавляет PKCS#7-паддинг до кратного 16 байт.
//  Буфер ciphertext должен быть минимум plen + 16 байт.
// ──────────────────────────────────────────
int encrypt_message(const uint8_t *plaintext, int plen,
                    const uint8_t *key, const uint8_t *iv,
                    uint8_t *ciphertext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        print_openssl_error("EVP_CIPHER_CTX_new");
        return -1;
    }

    int len = 0;
    int ciphertext_len = 0;

    // Инициализируем контекст: AES-256-CBC, ключ и IV
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        print_openssl_error("EVP_EncryptInit_ex");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    // Шифруем основную часть данных
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plen) != 1) {
        print_openssl_error("EVP_EncryptUpdate");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    ciphertext_len = len;

    // Записываем финальный блок (паддинг)
    if (EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len) != 1) {
        print_openssl_error("EVP_EncryptFinal_ex");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    ciphertext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext_len;
}

// ──────────────────────────────────────────
//  decrypt_message
//
//  AES-256-CBC расшифровка через EVP_DecryptUpdate + EVP_DecryptFinal_ex.
//  OpenSSL автоматически снимает PKCS#7-паддинг.
// ──────────────────────────────────────────
int decrypt_message(const uint8_t *ciphertext, int clen,
                    const uint8_t *key, const uint8_t *iv,
                    uint8_t *plaintext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        print_openssl_error("EVP_CIPHER_CTX_new");
        return -1;
    }

    int len = 0;
    int plaintext_len = 0;

    // Инициализируем контекст для расшифровки
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        print_openssl_error("EVP_DecryptInit_ex");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    // Расшифровываем основную часть
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, clen) != 1) {
        print_openssl_error("EVP_DecryptUpdate");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    plaintext_len = len;

    // Снимаем финальный блок (паддинг)
    if (EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len) != 1) {
        print_openssl_error("EVP_DecryptFinal_ex");
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    plaintext_len += len;

    // Добавляем нуль-терминатор для удобства работы со строками
    plaintext[plaintext_len] = '\0';

    EVP_CIPHER_CTX_free(ctx);
    return plaintext_len;
}
