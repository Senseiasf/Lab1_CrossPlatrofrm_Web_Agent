/**
 * @file logger.cpp
 * @brief Модуль ведения системных журналов (логов).
 * Обеспечивает централизованную и потокобезопасную запись событий работы агента.
 */
#include "logger.h"
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;
/** @brief Глобальный мьютекс для синхронизации записи в файл из разных потоков. */
std::mutex log_mtx;


/**
 * @brief Обеспечивает наличие папки для хранения логов.
 * @return fs::path Путь к директории "logs" относительно исполняемого файла.
 * * Логика: Проверяет существование папки "logs" в текущем рабочем каталоге.
 * Если папка отсутствует, создает её рекурсивно. Почему рекурсивно - не знаю.
 * Но работает именно так.
 */
fs::path get_log_directory() {
    fs::path log_dir = fs::current_path() / "logs";
    if (!fs::exists(log_dir)) {
        fs::create_directories(log_dir);
    }
    return log_dir;
}

/**
 * @brief Запись типового сообщения в системный лог.
 * @param module Имя модуля или уровень важности (напр., "SYSTEM", "ERROR", "UPLOADER").
 * @param message Текст сообщения для записи.
 * * * Особенности реализации:
 * 1. Thread-Safety: Использует std::lock_guard для предотвращения состояния гонки (Race Condition)
 * между рабочими потоками при обращении к одному файлу.
 * 2. Формат записи: [Дата Время] [ID Потока] [Модуль] Сообщение.
 * 3. Автономность: Сама открывает и закрывает файл в режиме std::ios::app (дозапись в конец),
 * что гарантирует сохранность данных при внезапном завершении программы.
 */
void log_message(const std::string& module, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mtx);

    fs::path log_file_path = get_log_directory() / "agent.log";
    std::ofstream log_file(log_file_path, std::ios::app);

    if (log_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        log_file << "[" << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S") << "] ";
        log_file << "[Thread: " << std::this_thread::get_id() << "] ";
        log_file << "[" << module << "] " << message << std::endl;

        log_file.close();
    } else {
        std::cerr << "[CRITICAL ERROR] Не удалось открыть лог-файл: " << log_file_path << std::endl;
    }
}
