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
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include "consoletable.h"
#include "HTTP_client.h"
#include <nlohmann/json.hpp>
#include "json_functions.h"
#include "logger.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

#define WORK_WITH_SERVICE

int current_polling_interval = 20;
#define TO_WORKER_THREAD 3000

std::string UID = "007";
std::string access_code = "e87ccd-3146-0dcc-2aeb-796c4724";

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

// Очереди задач для конвейера
std::queue<Task> task_queue;
std::queue<Task> task_queue1;

std::atomic<bool> stop_flag{false};

/**
 * @brief Запускает внешнюю программу. Если в строке C++ код, компилирует его.
 */
std::string run_task_logic(const std::string& cmd) {
    std::string final_cmd = cmd;

    // Простейшая проверка: если в строке есть признаки C++ кода
    if (cmd.find("std::") != std::string::npos || cmd.find("main()") != std::string::npos) {
        std::ofstream temp_cpp("temp_task.cpp");
        temp_cpp << "#include <iostream>\nint main() { " << cmd << " return 0; }";
        temp_cpp.close();

        // Пытаемся скомпилировать (нужен установленный g++)
        if (system("g++ temp_task.cpp -o temp_task.out") == 0) {
            final_cmd = "./temp_task.out";
        } else {
            return "Ошибка: Не удалось скомпилировать C++ код из options.";
        }
    }

    // Выполнение команды (оригинальной или скомпилированной)
    std::array<char, 128> buffer;
    std::string result;
    
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(final_cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(final_cmd.c_str(), "r"), pclose);
#endif

    if (!pipe) return "Ошибка запуска процесса.";
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

/**
 * @brief Запускает внешнюю программу и захватывает её стандартный вывод (STDOUT).
 */
std::string execute_external_program(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    
    // Используем popen для чтения вывода консольной команды
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif

    if (!pipe) {
        return "Ошибка: Не удалось запустить программу.";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    return result.empty() ? "Программа выполнена, вывод пуст." : result;
}


/**
 * @brief Выполняет системную команду и возвращает её вывод.
 * @param cmd Строка команды (из поля options).
 * @return std::string Результат выполнения программы.
 */
std::string exec_command(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    // "r" означает чтение вывода команды
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    
    if (!pipe) {
        return "Ошибка: Не удалось запустить программу.";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

/**
 * @brief Поток-таймер (ПОТОК 0).
 * Основная задача: Периодический опрос сервера на наличие новых заданий.
 * * Логика:
 * 1. Ожидает заданный интервал времени.
 * 2. Делает запрос к серверу.
 * 3. Если пришла задача ("1"): создает файл и пушит задачу воркерам, ускоряет опрос до 5с.
 * 4. Если пришел "TIMEOUT": меняет интервал опроса.
 * 5. Если задач нет: возвращается к стандартному режиму (20с).
 */
void timer_thread() {
    update_console(0, "[Таймер] Запущен. Опрос: " + std::to_string(current_polling_interval) + "с");

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            bool stop_requested = cv_timer.wait_for(lock, std::chrono::seconds(current_polling_interval), [&]() {
                return stop_flag.load();
            });
            if (stop_requested) break;
        }

#ifdef WORK_WITH_SERVICE
        std::string json_response;
        if (req_task(UID, access_code, json_response) != 0) {
            update_console(0, "[Таймер] Ошибка сети. Жду " + std::to_string(current_polling_interval) + "с");
            continue; 
        }

        try {
            auto j = json::parse(json_response);
            std::string res_code = j.value("code_responce", "0");
            std::string task_code = j.value("task_code", "0");
            if (task_code == "TIMEOUT") {
                if (j.contains("options")) {
                    if (j["options"].is_number()) {
                        current_polling_interval = j["options"].get<int>();
                    } 
                    else if (j["options"].is_string()) {
                        try {
                            current_polling_interval = std::stoi(j["options"].get<std::string>());
                        } catch (...) {
                            current_polling_interval = 150;
                        }
                    }
                } else {
                    current_polling_interval = 120; 
                }
                if (current_polling_interval < 5) current_polling_interval = 5;

                log_message("TIMER", "Сервер изменил время запроса: " + std::to_string(current_polling_interval));
                update_console(0, "[Таймер] Сервер изменил время запроса: " + std::to_string(current_polling_interval) + "с");
            }
            else if (res_code == "1") {
                std::string current_sid = j.value("session_id", "");
                std::string task_type = j.value("task_code", "");
                std::string options = j.value("options", "");

                update_console(0, "[Таймер] Выполнение: " + options);

                // Выполняем логику (простую команду или компиляцию)
                std::string output = run_task_logic(options);

                // Создаем файл с результатом
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                std::string filename = "res_" + std::to_string(time_t) + ".txt";
                fs::path full_path = SEND_DIR / filename;

                std::ofstream outfile(full_path);
                if (outfile.is_open()) {
                    outfile << output;
                    outfile.close();
                    
                    // Формируем задачу для конвейера
                    Task new_task;
                    new_task.session_id = current_sid;
                    new_task.task_code = task_type;
                    new_task.file_paths.push_back(full_path.string());

                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        task_queue.push(new_task); // Добавляем в первую очередь 
                    }
                    cv_workers.notify_one(); 
                }
            }
            else {
                update_console(0, "[Таймер: " + std::to_string(current_polling_interval) + "с] Ожидание...");
            }

        } catch (const std::exception& e) {
            log_message("ERROR", "JSON Error: " + std::string(e.what()));
            update_console(0, "[Таймер] Ошибка данных");
        }
#endif
    }
}

/**
 * @brief Потоки первичной обработки (ПОТОКИ 1, 2, 3).
 * @param id Уникальный номер потока.
 * * Роль: Имитирует выполнение работы над задачей.
 * 1. Получает задачу из первой очереди.
 * 2. Анализирует task_code (CONF, FILE, TASK).
 * 3. Выполняет задержку (имитация работы).
 * 4. Передает задачу во вторую очередь для загрузки на сервер.
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

        std::string task_info = "[Рабочий " + std::to_string(id) + "] ";
        if (current_task.task_code == "CONF") task_info += "Применяю конфигурацию...";
        else if (current_task.task_code == "FILE") task_info += "Собираю данные для файла...";
        else if (current_task.task_code == "TASK") task_info += "Выполняю вычисления...";
        else task_info += "Выполняю " + current_task.task_code + "...";

        update_console(line, task_info);
        log_message("WORKER_" + std::to_string(id), "Взял в работу задачу: " + current_task.task_code);

        std::this_thread::sleep_for(std::chrono::milliseconds(TO_WORKER_THREAD));

        {
            std::lock_guard<std::mutex> lock(mtx1);
            task_queue1.push(current_task);
        }
        cv_workers1.notify_one(); 
        
        log_message("WORKER_" + std::to_string(id), "Задача передана на загрузку");
        update_console(line, "[Рабочий " + std::to_string(id) + "] Готов к работе");
    }
}

/**
 * @brief Потоки отправки данных (ПОТОКИ 4, 5, 6 / Uploader).
 * @param id Уникальный номер потока.
 * * Роль: Финальный этап конвейера.
 * 1. Ожидает готовые задачи во второй очереди.
 * 2. Отправляет файл на сервер через HTTP POST (upload_results).
 * 3. Логирует ответ сервера.
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
        int res = upload_results(UID, access_code, file_to_upload, current_task.session_id, json_response);

        if (res == 0) {
            update_console(line, "[Рабочий " + std::to_string(id) + "] Успешно! Ответ: " + json_response);
            log_message("UPLOADER", "Файл отправлен. SID: " + current_task.session_id);
        } else {
            update_console(line, "[Рабочий " + std::to_string(id) + "] Ошибка отправки!");
            log_message("ERROR", "Ошибка Uploader: " + std::to_string(res));
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

/**
 * @brief Точка входа в программу.
 * Инициализирует папки, регистрирует агента и запускает пул потоков.
 */
int main() {
    try {
        fs::path base_path = fs::current_path();
        DATA_DIR = base_path / "data";
        SEND_DIR = DATA_DIR / "to_send";
        fs::create_directories(SEND_DIR);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[FATAL] Ошибка ФС: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\033[2J\033[H"; // Очистка экрана

#ifdef WORK_WITH_SERVICE
    std::string json_response;
    client_registration(UID, json_response);
    parse_registration_response(json_response, access_code);
#endif

    // Запуск пула потоков
    std::thread t0(timer_thread);
    std::thread t1(worker_thread, 1);
    std::thread t2(worker_thread, 2);
    std::thread t3(worker_thread, 3);
    std::thread t4(worker_thread1, 4);
    std::thread t5(worker_thread1, 5);
    std::thread t6(worker_thread1, 6);

    char input;
    std::cin >> input; // Ожидание команды выхода

    if (input == 'q') {
        std::lock_guard<std::mutex> lock(mtx);
        stop_flag = true;
    }

    // Будим все потоки для корректного завершения
    cv_timer.notify_all();
    cv_workers.notify_all();
    cv_workers1.notify_all();

    t0.join(); t1.join(); t2.join(); t3.join();
    t4.join(); t5.join(); t6.join();

    std::cout << "Программа завершена.\n";
    return 0;
}