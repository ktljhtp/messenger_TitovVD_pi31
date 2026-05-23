#pragma once

#include <stdint.h>

// Длина ключа AES-256 и вектора инициализации (в байтах)
#define AES_KEY_LEN 32
#define AES_IV_LEN  16

// ──────────────────────────────────────────
//  encrypt_message
//
//  Шифрует plaintext длиной plen байт
//  алгоритмом AES-256-CBC.
//
//  Параметры:
//    plaintext  — исходные данные
//    plen       — длина исходных данных (байт)
//    key        — ключ шифрования (32 байта)
//    iv         — вектор инициализации (16 байт)
//    ciphertext — буфер для результата
//                 (минимум plen + AES_BLOCK_SIZE байт)
//
//  Возвращает: длину зашифрованных данных, или -1 при ошибке.
// ──────────────────────────────────────────
int encrypt_message(const uint8_t *plaintext, int plen,
                    const uint8_t *key, const uint8_t *iv,
                    uint8_t *ciphertext);

// ──────────────────────────────────────────
//  decrypt_message
//
//  Расшифровывает ciphertext длиной clen байт.
//
//  Параметры:
//    ciphertext — зашифрованные данные
//    clen       — длина зашифрованных данных (байт)
//    key        — ключ шифрования (32 байта)
//    iv         — вектор инициализации (16 байт)
//    plaintext  — буфер для результата
//
//  Возвращает: длину расшифрованных данных, или -1 при ошибке.
// ──────────────────────────────────────────
int decrypt_message(const uint8_t *ciphertext, int clen,
                    const uint8_t *key, const uint8_t *iv,
                    uint8_t *plaintext);
