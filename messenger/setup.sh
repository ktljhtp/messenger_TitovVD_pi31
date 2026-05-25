#!/bin/bash
# ══════════════════════════════════════════════════════════
#  setup.sh — Этап 6: инициализация проекта
#
#  Что делает:
#    1. Проверяет все зависимости (gcc, openssl, ncurses)
#    2. Создаёт нужные папки
#    3. Генерирует data/users.dat с настоящими SHA-256 хешами
#    4. Создаёт data/groups.dat с тестовыми группами
#    5. Собирает весь проект через make
#    6. Запускает тест шифрования
#
#  Запуск: bash setup.sh
#  Или:    chmod +x setup.sh && ./setup.sh
# ══════════════════════════════════════════════════════════

set -e  # остановить скрипт при любой ошибке

# ── Цвета для вывода ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # сброс цвета

ok()   { echo -e "${GREEN}[✓]${NC} $1"; }
fail() { echo -e "${RED}[✗]${NC} $1"; exit 1; }
info() { echo -e "${CYAN}[→]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }

echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${BOLD}   Мессенджер — Инициализация проекта     ${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo ""

# ──────────────────────────────────────────
#  ШАГ 1: Проверка зависимостей
# ──────────────────────────────────────────
info "Шаг 1: Проверка зависимостей..."

check_dep() {
    local name="$1"
    local cmd="$2"
    if eval "$cmd" &>/dev/null; then
        ok "$name найден"
    else
        warn "$name не найден — устанавливаем..."
        sudo apt-get install -y "$3" 2>/dev/null || fail "Не удалось установить $name"
        ok "$name установлен"
    fi
}

check_dep "g++"         "g++ --version"            "g++"
check_dep "make"        "make --version"            "make"
check_dep "OpenSSL dev" "pkg-config --exists openssl" "libssl-dev"
check_dep "ncurses dev" "pkg-config --exists ncurses" "libncurses-dev"
check_dep "sha256sum"   "sha256sum --version"       "coreutils"
check_dep "nc (netcat)" "which nc"                  "netcat-openbsd"

echo ""

# ──────────────────────────────────────────
#  ШАГ 2: Создание структуры папок
# ──────────────────────────────────────────
info "Шаг 2: Создание структуры папок..."

mkdir -p server client common data
ok "Папки: server/ client/ common/ data/"

# ──────────────────────────────────────────
#  ШАГ 3: Генерация data/users.dat
# ──────────────────────────────────────────
info "Шаг 3: Генерация data/users.dat..."

# Функция генерации SHA-256 хеша пароля
hash_pass() {
    echo -n "$1" | sha256sum | cut -d' ' -f1
}

# Тестовые пользователи: логин | пароль | отображаемое имя
declare -A USERS
USERS["alice"]="alice123:Алиса"
USERS["bob"]="bob123:Боб"
USERS["charlie"]="charlie123:Чарли"

# Перезаписываем users.dat
cat > data/users.dat << 'HEADER'
# Формат: логин:sha256_хеш_пароля:отображаемое_имя
# Для добавления пользователя:
#   echo -n "ПАРОЛЬ" | sha256sum | cut -d' ' -f1
HEADER

for login in "${!USERS[@]}"; do
    IFS=':' read -r password display_name <<< "${USERS[$login]}"
    hash=$(hash_pass "$password")
    echo "${login}:${hash}:${display_name}" >> data/users.dat
    ok "  Пользователь: ${login} / пароль: ${password}"
done

echo ""

# ──────────────────────────────────────────
#  ШАГ 4: Создание data/groups.dat
# ──────────────────────────────────────────
info "Шаг 4: Создание data/groups.dat..."

cat > data/groups.dat << 'EOF'
# Формат: название_группы:логин1:логин2:...
testgroup:alice:bob:charlie
devteam:alice:bob
EOF

ok "Группы: testgroup (alice, bob, charlie), devteam (alice, bob)"
echo ""

# ──────────────────────────────────────────
#  ШАГ 5: Сборка проекта
# ──────────────────────────────────────────
info "Шаг 5: Сборка проекта (make all)..."
echo ""

if make all; then
    echo ""
    ok "Сервер собран → ./messenger_server"
    ok "Клиент собран → ./messenger_client"
else
    fail "Сборка не удалась — проверь ошибки выше"
fi
echo ""

# ──────────────────────────────────────────
#  ШАГ 5.5: Создание client.conf
# ──────────────────────────────────────────
info "Создание client.conf..."
if [[ ! -f "client.conf" ]]; then
cat > client.conf << CONF
# Адрес и порт сервера мессенджера
server=127.0.0.1
port=8080
CONF
    ok "client.conf создан (сервер: 127.0.0.1:8080)"
    warn_msg "Отредактируй client.conf если сервер на другой машине"
else
    ok "client.conf уже существует"
fi
echo ""

# ──────────────────────────────────────────
#  ШАГ 6: Тест шифрования
# ──────────────────────────────────────────
info "Шаг 6: Тест модуля шифрования..."
echo ""

if make test_crypto; then
    echo ""
    ok "Модуль шифрования работает корректно"
else
    fail "Тест шифрования провалился"
fi
echo ""

# ──────────────────────────────────────────
#  Итог
# ──────────────────────────────────────────
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}   Проект готов к запуску!${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo ""
echo -e "  Запуск сервера : ${CYAN}./messenger_server 8080${NC}"
echo -e "  Запуск клиента : ${CYAN}./messenger_client 127.0.0.1 8080${NC}"
echo ""
echo -e "  Тестовые пользователи:"
echo -e "    ${CYAN}alice${NC}   / alice123"
echo -e "    ${CYAN}bob${NC}     / bob123"
echo -e "    ${CYAN}bob${NC}     / charlie123"
echo ""
echo -e "  Для полного тестирования:"
echo -e "    ${CYAN}bash test_all.sh${NC}"
echo ""