#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <chrono>
#include <atomic>
#include <sstream>
#include <fstream>
#include <vector>
#include <filesystem>
#include "consoletable.h"
#include "HTTP_client.h"
#include <nlohmann/json.hpp>
#include "json_functions.h"
#include "logger.h"



using json = nlohmann::json;
namespace fs = std::filesystem;

//#define DEBUG_WO_THREADS
#define WORK_WITH_SERVICE

int current_polling_interval = 20;
#define TO_WORKER_THREAD 3000

std::string UID = "007";
// Глобальный код доступа. Он будет перезаписан при успешной регистрации.
std::string access_code = "e87ccd-3146-0dcc-2aeb-796c4724";

// Глобальные пути
fs::path DATA_DIR;
fs::path SEND_DIR;

// Структура для передачи задачи между потоками
struct Task {
    std::string session_id;
    std::string task_code;
    std::vector<std::string> file_paths;
};

// Глобальные объекты синхронизации
std::mutex mtx;
std::mutex mtx1;
std::condition_variable cv_timer;
std::condition_variable cv_workers;
std::condition_variable cv_workers1;

// Очереди задач
std::queue<Task> task_queue;
std::queue<Task> task_queue1;

std::atomic<bool> stop_flag{false};

/**
 * @brief Поток-таймер (ПОТОК 0).
 * Основная задача: Периодический опрос сервера на наличие новых заданий.
 * * Логика:
 * 1. Ждет заданный интервал (current_polling_interval).
 * 2. Делает запрос к API через req_task.
 * 3. Если code_responce == "1": создает файл-заглушку и отправляет задачу в task_queue.
 * 4. Управляет динамическим интервалом опроса (ускоряется при наличии работы в 4 раза
 * - с 20 секунд до 5. Добавил для себя, чтобы меньше ждать, пока тестировал. Ну и вообще
 * это удобно).
 */
void timer_thread() {
    update_console(0, "[Таймер] Запущен. Опрос каждые " + std::to_string(current_polling_interval) + "с");
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            // Ждем истечения интервала или сигнала от Uploader'а (сразу проверить сервер)
            bool stopped = cv_timer.wait_for(lock, std::chrono::seconds(current_polling_interval), [&]() {
                return stop_flag.load();
            });
            if (stopped) break;
        }

#ifdef WORK_WITH_SERVICE
        std::string json_response;
        req_task(UID, access_code, json_response);

        try {
            auto j = json::parse(json_response);
            std::string res_code = j.value("code_responce", "0");

            // Если есть задание (code_responce == "1")
            if (res_code == "1") {
                std::string current_sid = j.value("session_id", "");
                std::string task_type = j.value("task_code", "");

                // Создаем локальный файл для имитации сбора данных
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                std::string filename = "file_" + std::to_string(time_t) + ".txt";
                fs::path full_path = SEND_DIR / filename;

                std::ofstream outfile(full_path);
                if (outfile.is_open()) {
                    outfile << "Задача: " << task_type << "\nСессия: " << current_sid << "\n";
                    log_message("TIMER", "Получена задача: " + task_type + " | Сессия: " + current_sid);
                    outfile.close();
                } else {
                    log_message("ERROR", "Не удалось создать файл: " + full_path.string());
                    continue;
                }

                // Упаковываем в структуру Task
                Task new_task;
                new_task.session_id = current_sid;
                new_task.task_code = task_type;
                new_task.file_paths.push_back(full_path.string());

                // Отправляем рабочим потокам
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    task_queue.push(new_task);
                }
                cv_workers.notify_one();

                update_console(0, "[Таймер] Получено: " + task_type + " | Файл: " + filename);

                // Ускоряем опрос, пока есть задачи
                current_polling_interval = 5;
            }
            else {
                // Если задач нет (WAIT или "0"), возвращаемся к редкому опросу
                current_polling_interval = 20;
                update_console(0, "[Status] Waiting for tasks...");
            }
        } catch (const std::exception& e) {
            update_console(0, "[Таймер] Ошибка парсинга: " + std::string(e.what()));
            log_message("ERROR", "Таймер выбросил исключение: " + std::string(e.what()));
        }
#endif
    }
}

/**
 * @brief Потоки первичной обработки (ПОТОКИ 1, 2, 3).
 * @param id Уникальный номер потока для вывода в консоль.
 * * Роль в системе: Имитируют выполнение тяжелых вычислений или сбор данных.
 * * Да, всё ещё имитируют. Ну, спят несколько секунд и потом отправляют на сервер
 * * файл. В файле текст просто номер сессии там и ещё филер какой-то
 * *
 * * Логика:
 * 1. Спит на condition_variable, пока task_queue пуста.
 * 2. Получив задачу, определяет её тип (CONF, FILE, TASK - да, CONF есть,
 * но он её никак не обрабатывает. Просто выводит в консоль, что оно есть).
 * 3. Выполняет "работу" (задержка TO_WORKER_THREAD).
 * 4. Передает обработанную задачу во вторую очередь (task_queue1) для загрузчиков.
 * 5. Сбрасывает статус в консоли на "Готов к работе".
 */
void worker_thread(int id) {
    int line = id;
    update_console(line, "[Рабочий " + std::to_string(id) + "] Готов к работе");

    while (true) {
        Task current_task;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv_workers.wait(lock, [&]() {
                return !task_queue.empty() || stop_flag.load();
            });
            if (stop_flag.load() && task_queue.empty()) break;

            current_task = task_queue.front();
            task_queue.pop();
        }

        // ЛОГИКА "ПОНИМАНИЯ" ЗАДАНИЯ
        std::string task_info = "[Рабочий " + std::to_string(id) + "] ";
        if (current_task.task_code == "CONF") {
            task_info += "Применяю конфигурацию...";
        }
        else if (current_task.task_code == "FILE") {
            task_info += "Собираю данные для файла...";
        }
        else if (current_task.task_code == "TASK") {
            task_info += "Выполняю вычисления...";
        }
        else {
            task_info += "Выполняю " + current_task.task_code + "...";
        }

        update_console(line, task_info);

        // Имитация реальной работы
        std::this_thread::sleep_for(std::chrono::milliseconds(TO_WORKER_THREAD));

        // Передаем на этап загрузки
        {
            std::lock_guard<std::mutex> lock(mtx1);
            task_queue1.push(current_task);
        }
        cv_workers1.notify_one();
        update_console(line, "[Рабочий " + std::to_string(id) + "] Готов к работе");
    }
}

/**
 * @brief Потоки отправки данных (ПОТОКИ 4, 5, 6 / Uploader).
 * @param id Уникальный номер потока.
 * * Роль в системе: Последний этап конвейера. Отвечает за взаимодействие с сервером.
 * * Логика:
 * 1. Ожидает появления задач в task_queue1.
 * 2. Извлекает путь к файлу и session_id.
 * 3. Вызывает upload_results для передачи файла на сервер.
 * 4. Выводит ответ сервера (json_response) в консоль для контроля статуса "ok".
 */
void worker_thread1(int id) {
    int line = id;
    update_console(line, "[Рабочий " + std::to_string(id) + "] Запущен");

    while (true) {
        Task current_task;
        {
            std::unique_lock<std::mutex> lock(mtx1);
            cv_workers1.wait(lock, [&]() {
                return !task_queue1.empty() || stop_flag.load();
            });
            if (stop_flag.load() && task_queue1.empty()) break;

            current_task = task_queue1.front();
            task_queue1.pop();
        }

        std::string file_to_upload = current_task.file_paths.empty() ? "" : current_task.file_paths[0];
        update_console(line, "[Рабочий " + std::to_string(id) + "] Отправка: " + fs::path(file_to_upload).filename().string());

#ifdef WORK_WITH_SERVICE
        std::string json_response;
        // Передаем фактический путь и session_id конкретно этой задачи!
        int res = upload_results(UID, access_code, file_to_upload, current_task.session_id, json_response);

        if (res == 0) {
            update_console(line, "[Рабочий " + std::to_string(id) + "] Успешно отправлено! Ответ сервера: " + json_response);
            log_message("UPLOADER", "Файл успешно отправлен. Сессия: " + current_task.session_id);
            // ВАЖНО: Будим таймер, чтобы он МГНОВЕННО спросил сервер о новой задаче
        } else {
            update_console(line, "[Рабочий " + std::to_string(id) + "] Ошибка отправки!");
            log_message("ERROR", "Ошибка отправки файла " + file_to_upload + ". Код ошибки: " + std::to_string(res));
        }
#endif
        // Небольшая задержка перед следующей отправкой, чтобы не перегружать сеть
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

/**
 * @brief Точка входа в программу.
 * * Выполняет:
 * 1. Инициализацию файловой системы (создание папок data/to_send).
 * 2. Регистрацию клиента в системе (client_registration) и получение access_code.
 * 3. Запуск пула потоков (1 таймер, 3 рабочих, 3 загрузчика).
 * 4. Организацию корректного завершения работы через ввод символа 'q'.
 * * @return int Статус завершения (0 - успех, 1 - ошибка).
 */
int main() {
    // 1. Инициализация путей
    try {
        fs::path base_path = fs::current_path();
        DATA_DIR = base_path / "data";
        SEND_DIR = DATA_DIR / "to_send";
        fs::create_directories(SEND_DIR);
        log_message("SYSTEM", "Директории инициализированы: " + SEND_DIR.string());
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[FATAL] Ошибка ФС: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\033[2J\033[H";

#ifdef WORK_WITH_SERVICE
    std::string json_response;

    client_registration(UID, json_response);

    // ВАЖНО: Обновляем ГЛОБАЛЬНУЮ переменную access_code
    switch (parse_registration_response(json_response, access_code)) {
        case 0:
            std::cout << "Registration OK. Access code updated.\n";
            log_message("SYSTEM", "Registration OK. Access code updated.");
            break;
        case -3:
            std::cout << "Already registered. Using existing access code.\n";
            log_message("SYSTEM", "Already registered. Using existing access code.");
            break;
        default:
            std::cout << "Registration failed.\n";
            log_message("ERROR", "Registration failed.");
            break;
    }
#endif

#ifndef DEBUG_WO_THREADS
    std::thread t0(timer_thread);
    std::thread t1(worker_thread, 1);
    std::thread t2(worker_thread, 2);
    std::thread t3(worker_thread, 3);
    std::thread t4(worker_thread1, 4);
    std::thread t5(worker_thread1, 5);
    std::thread t6(worker_thread1, 6);

    char input;
    std::cin >> input;

    if (input == 'q') {
        std::cout << "\n\n\n\n\n\nНажата 'q', завершаем программу...\n";
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        stop_flag = true;
    }
    cv_timer.notify_all();
    cv_workers.notify_all();
    cv_workers1.notify_all();

    t0.join();
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();

    std::cout << "Все потоки завершены, программа закрывается\n";
#else
    timer_thread();
    worker_thread(1);
    worker_thread1(4);
    std::cout << "\n\n\n\nВсе потоки завершены, программа закрывается\n";
#endif
    return 0;
}
