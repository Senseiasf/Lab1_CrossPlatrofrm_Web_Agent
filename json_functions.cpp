/**
 * @file json_functions.cpp
 * @brief Модуль обработки JSON-ответов от сервера.
 * Выполняет десериализацию данных и извлечение управляющих параметров (токены, ID сессий).
 */
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include "json_functions.h" // Ensure this header matches the signatures
#include "logger.h"

using json = nlohmann::json;

/**
 * @brief Парсинг ответа при регистрации агента.
 * @param jsonResponse Необработанный JSON-текст от сервера.
 * @param out_access_code [out] Ссылка для сохранения полученного кода доступа.
 * @return int Статус регистрации:
 * 0 - Успешная новая регистрация.
 * -3 - Агент уже зарегистрирован (используем существующий код).
 * -1 - Ошибка парсинга или отсутствие обязательных полей.
 * * Логика: Извлекает "access_code", который необходим для всех последующих
 * авторизованных запросов к API.
 */
int parse_registration_response(const std::string& jsonResponse, std::string& out_access_code)
{
    try {
        json data = json::parse(jsonResponse);

        // Check if the key exists to prevent crashes
        if (!data.contains("code_responce")) return -1;

        std::string code = data["code_responce"].get<std::string>();

        if (code == "0" || code == "-3") {
            if (data.contains("access_code")) {
                // Correctly assign to the reference parameter
                out_access_code = data["access_code"].get<std::string>();
            }

            #ifdef SEQ_OUT
            if (data.contains("msg")) std::cout << data["msg"] << std::endl;
            #endif

            return (code == "0") ? 0 : -3;
        }
    }
    catch (const json::exception& e) {
        log_message("JSON_ERROR", "JSON Error during registration: " + std::string(e.what()));
        return -1;
    }
    return -1;
}

/**
 * @brief Парсинг ответа при опросе сервера на наличие задач.
 * @param jsonResponse Необработанный JSON-текст от сервера.
 * @param out_session_id [out] Ссылка для сохранения ID текущей сессии задачи.
 * @return int Статус задачи:
 * 1 - Новая задача получена.
 * 0 - Задач на сервере нет (режим ожидания).
 * -1 - Ошибка формата JSON или сетевого протокола.
 * * Логика: Проверяет "code_responce". При значении "1" фиксирует "session_id",
 * который связывает процесс обработки файла и последующую отправку результата.
 */
int parse_req_task(const std::string& jsonResponse, std::string& out_session_id)
{
    try {
        json data = json::parse(jsonResponse);

        if (!data.contains("code_responce")) return -1;

        std::string code = data["code_responce"].get<std::string>();

        if (code == "1") {
            if (data.contains("session_id")) {
                // Save session_id to the passed reference
                out_session_id = data["session_id"].get<std::string>();
            }
            return 1;
        }
        if (code == "0") return 0;
    }
    catch (...) {
        return -1;
    }
    return -1;
}
