// sender_thread больше не используется:
// отправка сообщений встроена в ui_run() внутри ui.cpp.
// Файл оставлен для совместимости со структурой проекта.

void *sender_thread(void *) { return nullptr; }