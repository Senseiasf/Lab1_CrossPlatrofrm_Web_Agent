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
#include <deque>
#include <memory>
#include <stdexcept>
#include <array>
#include <map>
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
std::mutex auth_mtx; // Мьютекс для защиты UID и access_code
std::map<std::string, std::string> keychain;

fs::path DATA_DIR;
fs::path SEND_DIR;

// Структура для передачи задачи между потоками
struct Task {
    std::string session_id;
    std::string task_code;
    std::vector<std::string> file_paths;
    
    // ДОБАВЛЯЕМ: Контекст авторизации для конкретной задачи
    std::string task_uid; 
    std::string task_token;
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

    // 1. Проверка на наличие C++ кода
    if (cmd.find("std::") != std::string::npos || cmd.find("main()") != std::string::npos) {
        std::ofstream temp_cpp("temp_task.cpp");
        // Добавим базовые инклуды для стабильности
        temp_cpp << "#include <iostream>\n#include <vector>\n#include <string>\n";
        temp_cpp << "int main() { " << cmd << " return 0; }";
        temp_cpp.close();

        // 2. Настраиваем переменные окружения в зависимости от ОС
        std::string out_binary;
        std::string run_command;
        
#ifdef _WIN32
        out_binary = "temp_task.exe";
        run_command = out_binary; // В Windows текущая папка обычно в пути поиска
#else
        out_binary = "temp_task.out";
        run_command = "./" + out_binary; // В Linux обязателен префикс ./
#endif

        // Команда компиляции (перенаправляем stdout и stderr в лог)
        std::string compile_cmd = "g++ temp_task.cpp -o " + out_binary + " > compile_log.txt 2>&1";

        if (system(compile_cmd.c_str()) == 0) {
            final_cmd = run_command;
        } else {
            // Если компиляция упала, возвращаем лог ошибок
            std::ifstream err_file("compile_log.txt");
            std::string err_content((std::istreambuf_iterator<char>(err_file)),
                                     std::istreambuf_iterator<char>());
            
            if (err_content.empty()) err_content = "Неизвестная ошибка компиляции g++.";
            return "ОШИБКА КОМПИЛЯЦИИ C++:\n" + err_content;
        }
    }

    // 3. Выполнение команды (оригинальной или только что созданной)
    std::array<char, 128> buffer;
    std::string result;
    
#ifdef _WIN32
    // Windows: используем _popen и _pclose
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(final_cmd.c_str(), "r"), _pclose);
#else
    // Linux: добавляем 2>&1 прямо в команду, чтобы поймать runtime-ошибки
    std::string exec_cmd = final_cmd + " 2>&1";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(exec_cmd.c_str(), "r"), pclose);
#endif

    if (!pipe) return "Ошибка: Не удалось открыть канал (pipe) для выполнения.";
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    return result.empty() ? "Программа выполнена успешно (вывод пуст)." : result;
}

/**
 * @brief Запускает внешнюю программу и захватывает её стандартный вывод (STDOUT).
 */
std::string execute_external_program(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    
    // Используем popen для чтения вывода консольной команды
#ifdef _WIN32
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
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
#ifdef _WIN32
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(cmd, "r"), _pclose);
#else
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd, "r"), pclose);
#endif

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
/**
 * @brief Поток-таймер (ПОТОК 0).
 */
void timer_thread() {
    while (true) {
        // --- БЕЗОПАСНОЕ ЧТЕНИЕ UID ДЛЯ ИНТЕРФЕЙСА ---
        std::string current_uid_display;
        {
            std::lock_guard<std::mutex> auth_lock(auth_mtx);
            current_uid_display = UID;
        }
        
        // Обновляем статус ПЕРЕД уходом в сон
        update_console(0, "[Таймер | UID: " + current_uid_display + " | " + std::to_string(current_polling_interval) + "с] Ожидание...");

        {
            std::unique_lock<std::mutex> lock(mtx);
            bool stop_requested = cv_timer.wait_for(lock, std::chrono::seconds(current_polling_interval), [&]() {
                return stop_flag.load();
            });
            if (stop_requested) break;
        }

#ifdef WORK_WITH_SERVICE
        std::string json_response;
        
        // --- БЕЗОПАСНОЕ ЧТЕНИЕ АВТОРИЗАЦИИ ДЛЯ ЗАПРОСА ---
        std::string safe_uid, safe_access_code;
        {
            std::lock_guard<std::mutex> auth_lock(auth_mtx);
            safe_uid = UID;
            safe_access_code = access_code;
        }

        // Используем safe_uid и safe_access_code вместо глобальных
        if (req_task(safe_uid, safe_access_code, json_response) != 0) {
            update_console(0, "[Таймер | UID: " + safe_uid + "] Ошибка сети. Жду " + std::to_string(current_polling_interval) + "с");
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
                            current_polling_interval = 120;
                        }
                    }
                } else {
                    current_polling_interval = 120; 
                }
                
                if (current_polling_interval < 5) current_polling_interval = 5;

                log_message("TIMER", "Сервер изменил время запроса: " + std::to_string(current_polling_interval));
                update_console(0, "[Таймер | UID: " + safe_uid + "] Пауза от сервера: " + std::to_string(current_polling_interval) + "с");
                // Специально делаем sleep, чтобы надпись не затерлась мгновенно
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            else if (res_code == "1") {
                std::string current_sid = j.value("session_id", "");
                std::string task_type = j.value("task_code", "");
                std::string options = j.value("options", "");

                log_message("TIMER", "Новая задача: " + task_type);
                update_console(0, "[Таймер | UID: " + safe_uid + " | " + std::to_string(current_polling_interval) + "с] Отдал задачу воркеру");

                Task new_task;
                new_task.session_id = current_sid;
                new_task.task_code = task_type;
                new_task.file_paths.push_back(options); 
                
                // ДОБАВЛЕНО: Прикрепляем "паспорт" текущей сессии к задаче
                new_task.task_uid = safe_uid;
                new_task.task_token = safe_access_code;

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    task_queue.push(new_task); 
                }
                cv_workers.notify_one(); 

                current_polling_interval = 5; // Ускоряемся при наличии задач
                
                // Чтобы пользователь успел увидеть статус "Отдал задачу"
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            else {
                current_polling_interval = 20; 
            }

        } catch (const std::exception& e) {
            log_message("ERROR", "JSON Error: " + std::string(e.what()));
            update_console(0, "[Таймер | UID: " + safe_uid + "] Ошибка данных");
            std::this_thread::sleep_for(std::chrono::seconds(1));
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
/**
 * @brief Потоки первичной обработки (ПОТОКИ 1, 2, 3).
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

        // --- ВЫПОЛНЕНИЕ РЕАЛЬНОЙ ЛОГИКИ ---
        
        std::string command_to_run = current_task.file_paths.empty() ? "" : current_task.file_paths[0];
        current_task.file_paths.clear(); 
        
        std::string output = "";
        
        if (!command_to_run.empty() && current_task.task_code == "TASK") {
            output = run_task_logic(command_to_run);
        } 
        else if (current_task.task_code == "CONF") {
            if (!command_to_run.empty()) {
                std::string old_uid;
                std::string new_uid = command_to_run;
                int reg_status = -1;

                {
                    std::lock_guard<std::mutex> auth_lock(auth_mtx);
                    
                    old_uid = UID;
                    UID = new_uid; 

                    std::string new_json_response;
                    client_registration(UID, new_json_response);
                    
                    std::string temp_code;
                    reg_status = parse_registration_response(new_json_response, temp_code);

                    if (reg_status == 0) {
                        access_code = temp_code;
                        keychain[UID] = access_code; 
                    } 
                    else if (reg_status == -3) {
                        if (!temp_code.empty()) {
                            access_code = temp_code;
                            keychain[UID] = access_code;
                        } else if (keychain.count(UID)) {
                            access_code = keychain[UID];
                        } else {
                            reg_status = -1; 
                            UID = old_uid; 
                        }
                    } 
                    else {
                        UID = old_uid; 
                    }

                    if (reg_status == 0 || reg_status == -3) {
                        // Обновленный путь для сохранения
                        fs::path keychain_path = fs::current_path() / "keychain.json";
                        std::ofstream kc_out(keychain_path);
                        if (kc_out.is_open()) {
                            json j = keychain;
                            kc_out << j.dump(4);
                        }
                    }
                } 

                if (reg_status == 0) {
                    output = "Конфигурация успешно обновлена. Новый UID: " + new_uid + ". Получен новый access_code.";
                    log_message("WORKER_" + std::to_string(id), "Успешная регистрация нового UID: " + new_uid);
                } else if (reg_status == -3) {
                    output = "Конфигурация обновлена. Вернулись на UID: " + new_uid + ". Использован существующий access_code сервера.";
                    log_message("WORKER_" + std::to_string(id), "Установлен старый UID: " + new_uid);
                } else {
                    output = "Ошибка перерегистрации! Откат изменений. Текущий UID остался: " + old_uid;
                    log_message("ERROR", "Сбой конфигурации. Возврат к старому UID: " + old_uid);
                }
            } else {
                output = "Ошибка: Сервер прислал команду CONF без указания нового UID.";
            }
        }
        else if (current_task.task_code == "FILE") {
            fs::path logs_dir = fs::current_path() / "logs";
            std::string latest_log_path = "";
            auto last_time = fs::file_time_type::min();

            // Ищем самый свежий файл в папке logs
            if (fs::exists(logs_dir) && fs::is_directory(logs_dir)) {
                for (const auto& entry : fs::directory_iterator(logs_dir)) {
                    if (entry.is_regular_file()) {
                        auto ftime = fs::last_write_time(entry);
                        if (ftime > last_time) {
                            last_time = ftime;
                            latest_log_path = entry.path().string();
                        }
                    }
                }
            }

            if (!latest_log_path.empty()) {
                std::ifstream log_file(latest_log_path);
                if (log_file.is_open()) {
                    // Парсим количество строк из options (command_to_run)
                    int lines_to_read = -1; // -1 означает читать весь файл
                    if (!command_to_run.empty()) {
                        try {
                            lines_to_read = std::stoi(command_to_run);
                            if (lines_to_read <= 0) lines_to_read = -1; // Защита от дурака
                        } catch (...) {
                            lines_to_read = -1; // Если в options пришел мусор, читаем всё
                        }
                    }

                    if (lines_to_read == -1) {
                        // Читаем весь файл
                        output = std::string((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
                    } else {
                        // Читаем только последние N строк через скользящее окно
                        std::deque<std::string> buffer;
                        std::string line;
                        while (std::getline(log_file, line)) {
                            buffer.push_back(line);
                            if (buffer.size() > static_cast<size_t>(lines_to_read)) {
                                buffer.pop_front(); // Удаляем самую старую строку, если превысили лимит
                            }
                        }
                        // Склеиваем результат
                        for (const auto& l : buffer) {
                            output += l + "\n";
                        }
                    }
                    
                    log_message("WORKER_" + std::to_string(id), "Логи успешно считаны для отправки");
                } else {
                    output = "Ошибка: Файл логов найден (" + fs::path(latest_log_path).filename().string() + "), но не может быть прочитан.";
                    log_message("ERROR", "Не смог открыть лог-файл для чтения");
                }
            } else {
                output = "Ошибка: Папка logs пуста или не существует.";
                log_message("ERROR", "Команда FILE: логи не найдены");
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(TO_WORKER_THREAD));
            output = "Неизвестная команда выполнена (заглушка). Код: " + current_task.task_code;
        }

        // --- ФОРМИРОВАНИЕ ФАЙЛА РЕЗУЛЬТАТА ---
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::string filename = "res_" + std::to_string(time_t) + "_w" + std::to_string(id) + ".txt";
        fs::path full_path = SEND_DIR / filename;

        std::ofstream outfile(full_path);
        if (outfile.is_open()) {
            outfile << output;
            outfile.close();
            current_task.file_paths.push_back(full_path.string()); 
        }

        // --- ПЕРЕДАЧА В ОЧЕРЕДЬ ЗАГРУЗКИ ---
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
        // --- БЕЗОПАСНОЕ ЧТЕНИЕ АВТОРИЗАЦИИ ---
        std::string safe_uid, safe_access_code;
        {
            std::lock_guard<std::mutex> auth_lock(auth_mtx);
            safe_uid = UID;
            safe_access_code = access_code;
        }

        // Используем safe_uid и safe_access_code вместо глобальных
        int res = upload_results(safe_uid, safe_access_code, file_to_upload, current_task.session_id, json_response);

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

int main() {
    try {
        fs::path base_path = fs::current_path();
        DATA_DIR = base_path / "data";
        SEND_DIR = DATA_DIR / "to_send";
        fs::create_directories(SEND_DIR);
        
        // Задаем новый путь для файла ключей (в корне)
        fs::path keychain_path = base_path / "keychain.json";
        
        // --- ЗАГРУЗКА ПАМЯТИ АГЕНТА ИЗ ФАЙЛА ---
        std::ifstream kc_file(keychain_path);
        if (kc_file.is_open()) {
            json j;
            kc_file >> j;
            keychain = j.get<std::map<std::string, std::string>>();
        }
        // ---------------------------------------

    } catch (const std::exception& e) { // Ловим std::exception, чтобы поймать и ошибки json
        std::cerr << "[FATAL] Ошибка инициализации: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\033[2J\033[H"; // Очистка экрана

#ifdef WORK_WITH_SERVICE
    std::string json_response;
    client_registration(UID, json_response);

    std::string temp_code;
    int reg_status = parse_registration_response(json_response, temp_code);

    if (reg_status == 0) {
        // Успешная новая регистрация
        std::cout << "Registration OK. Access code updated.\n";
        log_message("SYSTEM", "Registration OK. Access code updated.");
        access_code = temp_code;
        keychain[UID] = access_code; 
    } 
    else if (reg_status == -3) {
        // Агент уже зарегистрирован
        std::cout << "Already registered. Verifying access code...\n";
        log_message("SYSTEM", "Already registered. Verifying access code...");
        
        if (!temp_code.empty()) {
            access_code = temp_code;
            keychain[UID] = access_code;
        } else if (keychain.count(UID)) {
            // Токен есть в локальной памяти
            access_code = keychain[UID];
            std::cout << "Access code loaded from local keychain.\n";
        } else {
            // Токена нет ни на сервере, ни в файле. 
            // НО у нас есть захардкоженный пароль в самом начале файла!
            // Используем его и обязательно сохраняем в память.
            std::cout << "[WARNING] Using default hardcoded access code for " << UID << "\n";
            log_message("SYSTEM", "Using hardcoded token for default UID: " + UID);
            keychain[UID] = access_code; 
        }
    } 
    else {
        std::cout << "Registration failed.\n";
        log_message("ERROR", "Registration failed.");
    }

    // --- СОХРАНЕНИЕ ПАМЯТИ АГЕНТА В ФАЙЛ ---
    if (reg_status == 0 || reg_status == -3) {
        fs::path keychain_path = fs::current_path() / "keychain.json";
        std::ofstream kc_out(keychain_path);
        if (kc_out.is_open()) {
            json j = keychain;
            kc_out << j.dump(4);
        }
    }
    // ---------------------------------------
#endif

    // Запуск пула потоков
    std::thread t0(timer_thread);
    std::thread t1(worker_thread, 1);
    std::thread t2(worker_thread, 2);
    std::thread t3(worker_thread, 3);
    std::thread t4(worker_thread1, 4);
    std::thread t5(worker_thread1, 5);
    std::thread t6(worker_thread1, 6);

    std::cout << "\n\n\n\n\n\n\n\n\n> Введите 'q' для выхода: ";
    
    char input;
    std::cin >> input; 

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

