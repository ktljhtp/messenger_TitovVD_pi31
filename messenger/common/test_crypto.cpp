// ──────────────────────────────────────────
//  test_crypto.cpp — изолированный тест модуля шифрования
//
//  Сборка:
//    g++ -std=c++17 test_crypto.cpp crypto.cpp -lssl -lcrypto -o test_crypto
//  Запуск:
//    ./test_crypto
// ──────────────────────────────────────────
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crypto.h"

int main()
{
    // Тестовый ключ и IV (в реальном проекте генерируются случайно)
    uint8_t key[AES_KEY_LEN] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    uint8_t iv[AES_IV_LEN] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };

    const char *original = "Привет, это тестовое сообщение мессенджера!";
    int orig_len = (int)strlen(original);

    printf("Исходный текст  : %s\n", original);
    printf("Длина           : %d байт\n\n", orig_len);

    // ── Шифрование ──
    // Буфер: исходная длина + 16 байт (максимальный паддинг AES)
    uint8_t ciphertext[1040];
    int cipher_len = encrypt_message(
        (const uint8_t *)original, orig_len,
        key, iv,
        ciphertext
    );

    if (cipher_len < 0) {
        fprintf(stderr, "ОШИБКА: encrypt_message вернул -1\n");
        return 1;
    }

    printf("Зашифровано     : %d байт\n", cipher_len);
    printf("Hex             : ");
    for (int i = 0; i < cipher_len; i++) {
        printf("%02x", ciphertext[i]);
    }
    printf("\n\n");

    // ── Расшифровка ──
    uint8_t decrypted[1040];
    int decrypted_len = decrypt_message(
        ciphertext, cipher_len,
        key, iv,
        decrypted
    );

    if (decrypted_len < 0) {
        fprintf(stderr, "ОШИБКА: decrypt_message вернул -1\n");
        return 1;
    }

    printf("Расшифровано    : %s\n", (char *)decrypted);
    printf("Длина           : %d байт\n\n", decrypted_len);

    // ── Проверка совпадения ──
    if (decrypted_len == orig_len &&
        memcmp(original, decrypted, orig_len) == 0) {
        printf("✓ ТЕСТ ПРОЙДЕН: расшифрованный текст совпадает с исходным\n");
        return 0;
    } else {
        fprintf(stderr, "✗ ТЕСТ ПРОВАЛЕН: тексты не совпадают!\n");
        return 1;
    }
}
