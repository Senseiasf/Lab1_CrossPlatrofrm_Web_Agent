#include <iostream>
#include <string>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include "consoletable.h"
#include <filesystem>
#include "logger.h"
namespace fs = std::filesystem;

using json = nlohmann::json;

/**
 * @brief Запрос нового задания у сервера (Polling).
 * @param uid Уникальный идентификатор агента.
 * @param access_code Действующий код доступа (токен).
 * @param json_response [out] Строка, куда будет записан необработанный ответ сервера.
 * @return int 0 при успешном запросе, 1 при ошибке соединения/API.
 * * Логика: формирует POST-запрос с метаданными агента. Если сервер возвращает статус 2xx,
 * записывает тело ответа в json_response для дальнейшего парсинга в таймер-потоке.
 */
int req_task(std::string & uid, std::string & access_code, std::string & json_response) {
    // URL для запроса
    //std::string url = "https://jsonplaceholder.typicode.com/posts";
    std::string url = "https://xdev.arkcom.ru:9999/app/webagent1/api/wa_task/";

    // JSON тело запроса

    std::string json_body;



    json j;
    j["UID"] = uid;
    j["descr"] = "web-agent";
    j["access_code"] = access_code;

    json_body = j.dump(4);


 #ifdef SEQ_OUT
    std::cout << "Sending request to: " << url << std::endl;
    std::cout << "JSON body: " << json_body << std::endl;
    std::cout << std::endl;
#endif

    // Выполняем POST запрос
    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json_body}
    );

    // Проверяем результат
    if (response.status_code >= 200 && response.status_code < 300) {
#ifdef SEQ_OUT
        std::cout << "Success! Status code: " << response.status_code << std::endl;
        std::cout << "Response: " << response.text << std::endl;
#endif
        json_response = response.text;
    } else {
#ifdef SEQ_OUT
        std::cout << "Error! Status code: " << response.status_code << std::endl;
#endif
        if (!response.error.message.empty()) {
#ifdef SEQ_OUT
            std::cout << "Error message: " << response.error.message << std::endl;
#endif
            return 1;
        }
    }

    return 0;
}

/**
 * @brief Первичная регистрация агента в системе.
 * @param UID Уникальный идентификатор устройства/агента.
 * @param json_response [out] Ответ сервера, содержащий новый access_code.
 * @return int 0 в случае успеха.
 * * Логика: выполняется один раз при запуске main(). Отправляет UID сервера для
 * получения или подтверждения прав доступа. Без успешного выполнения этой функции
 * дальнейшая работа (req_task) невозможна. В теории не должна была меняться
 * со времён Ивана (XVI век).
 */
int client_registration(std::string& UID,std::string& json_response) {
    // URL для запроса
    //std::string url = "https://jsonplaceholder.typicode.com/posts";
    std::string url = "https://xdev.arkcom.ru:9999/app/webagent1/api/wa_reg/";


    json j;
    j["UID"] = UID;
    j["descr"] = "web-agent";


    std::string json_body;
    json_body = j.dump(4);
 #ifdef SEQ_OUT
    std::cout << "Sending request to: " << url << std::endl;
    std::cout << "JSON body: " << json_body << std::endl;
    std::cout << std::endl;
#endif

    // Выполняем POST запрос
    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json_body}
    );

    // Проверяем результат
    if (response.status_code >= 200 && response.status_code < 300) {
    #ifdef SEQ_OUT
        std::cout << "Success! Status code: " << response.status_code << std::endl;
        std::cout << "Response: " << response.text << std::endl;
    #endif
        json_response = response.text;
    } else {
    #ifdef SEQ_OUT
        std::cout << "Error! Status code: " << response.status_code << std::endl;
    #endif
        if (!response.error.message.empty()) {
    #ifdef SEQ_OUT
            std::cout << "Error message: " << response.error.message << std::endl;
    #endif
            json_response = "";
        }
    }

    return 0;
}


/**
 * @brief Загрузка результатов выполнения задачи на сервер.
 * @param uid Идентификатор агента.
 * @param access_code Код доступа.
 * @param file_path Полный путь к локальному файлу, который нужно отправить.
 * @param session_id ID текущей сессии задания, полученный от сервера ранее.
 * @param json_response [out] Текстовый ответ сервера на загрузку.
 * @return int Статус операции:
 * 0  - Успешная загрузка и подтверждение от сервера.
 * -1  - Файл не найден локально.
 * -2  - Ошибка логики сервера (code_responce != 0).
 * -3  - Ошибка HTTP (статус не 2xx).
 * -4  - Критическое исключение (Exception).
 * * Логика: формирует сложный Multipart-запрос, содержащий:
 * 1. JSON-метаданные (result) с привязкой к session_id.
 * 2. Код результата (result_code).
 * 3. Бинарное содержимое файла (file1).
 * Лютейший вайбкод. ГОООООООООООООООООЛ
 */
int upload_results(const std::string& uid,
                   const std::string& access_code,
                   const std::string& file_path,
                   const std::string& session_id,
                   std::string& json_response)
{
    // 1. Подготовка JSON данных для поля "result"
    json j;
    j["UID"] = uid;
    j["access_code"] = access_code;
    j["message"] = "Task completed successfully";
    j["files"] = 1;
    j["session_id"] = session_id;

    std::string result_json_payload = j.dump();

    // 2. Проверка наличия файла
    fs::path p(file_path);
    if (!fs::exists(p)) {
        log_message("HTTP_ERROR", "File not found at " + file_path);
        return -1;
    }

    // 3. Формирование Multipart запроса
    // result_code: 0 для успеха
    // result: JSON строка с метаданными
    // file1: содержимое файла
    cpr::Multipart multipart_form{
        {"result_code", "0"},
        {"result", result_json_payload},
        {"file1", cpr::File{file_path}}
    };

    // 4. Отправка POST запроса
    try {
        cpr::Response response = cpr::Post(
            cpr::Url{"https://xdev.arkcom.ru:9999/app/webagent1/api/wa_result/"},
            multipart_form,
            cpr::Timeout{30000}
        );

        // 5. Разбор ответа
        if (response.status_code >= 200 && response.status_code < 300) {
            json_response = response.text;

            auto resp_j = json::parse(response.text);
            if (resp_j.value("code_responce", "-1") == "0") {
                return 0; // Успех
            } else {
                return -2; // Ошибка логики на стороне сервера
            }
        } else {
            log_message("HTTP_ERROR", "HTTP Error: " + std::to_string(response.status_code));
            return -3;
        }
    } catch (const std::exception& e) {
        log_message("CRITICAL", "Exception during upload: " + std::string(e.what()));
        return -4;
    }
}
