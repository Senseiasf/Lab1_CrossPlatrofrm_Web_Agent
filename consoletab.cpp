/**
 * @file consoletab.cpp
 * @brief Модуль управления консольным интерфейсом (TUI).
 * Обеспечивает статичное отображение статусов потоков с использованием ANSI-кодов.
 */
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <chrono>
#include <atomic>
#include <sstream>

#include "consoletable.h"



////////////////////////////////////////////////////////////////////////////

/**
 * @brief Вспомогательная функция формирования временной метки.
 * @return std::string Время в формате "Mon Jan 01 12:00:00".
 * * Логика: Использует ctime для получения читаемой даты, удаляя лишний символ переноса строки.
 */
std::string get_time_str() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::string time_str = ctime(&time_t);
    time_str.pop_back(); // удаляем \n
    return time_str;
}

// Глобальный мьютекс для синхронизации вывода
std::mutex console_mutex;

/** * @struct ConsoleTable
 * @brief Внутренняя структура для хранения и отрисовки строк таблицы.
 * Содержит вектор строк и механизмы управления курсором терминала.
 */
struct ConsoleTable {
    std::vector<std::string> lines;
    std::mutex mtx;

    /**
     * @brief Записывает текст в конкретную строку таблицы.
     * @param line_num Индекс строки (от 0).
     * @param text Содержание сообщения.
     * * Примечание: Автоматически расширяет вектор, если номер строки больше текущего размера.
     */
    void set_line(int line_num, const std::string& text) {
        std::lock_guard<std::mutex> lock(mtx);
        if (line_num >= lines.size()) {
            lines.resize(line_num + 1);
        }
        lines[line_num] = get_time_str()+" "+text;
    }

    /**
     * @brief Отрисовка всей таблицы в консоли.
     * * Применяемые ANSI-последовательности:
     * \033[s - сохранить позицию курсора.
     * \033[1;1H - переход в верхний левый угол (строка 1, столбец 1).
     * \033[K - очистка строки от курсора до конца.
     * \033[u - восстановить позицию курсора.
     */
    void render() {
        std::lock_guard<std::mutex> lock(mtx);

        // Сохраняем позицию курсора
        std::cout << "\033[s";

        // Перемещаемся в начало таблицы
        std::cout << "\033[1;1H";

        // Выводим все строки
        for (size_t i = 0; i < lines.size(); ++i) {
            // Очищаем строку
            std::cout << "\033[K";

            if (i < lines.size() && !lines[i].empty()) {
                std::cout << lines[i];
            }

            // Переходим на следующую строку
            if (i < lines.size() - 1) {
                std::cout << "\n";
            }
        }

        // Восстанавливаем позицию курсора
        std::cout << "\033[u";
        std::cout.flush();
    }
};

// Глобальная таблица
ConsoleTable table;

/**
 * @brief Публичный интерфейс для обновления информации в консоли.
 * @param line Номер строки (0 - Таймер, 1-3 - Рабочие, 4-6 - Загрузчики).
 * @param text Сообщение для вывода.
 * * Удостоверяется в безопасности потоков: Использует глобальный
 * * console_mutex для предотвращения артефактов отрисовки при
 * * одновременном вызове из разных потоков.
 */
void update_console(int line, const std::string& text) {

#ifndef SEQ_OUT
    std::lock_guard<std::mutex> lock(console_mutex);
    table.set_line(line, text);
    table.render();
#else
    //std::cout<<get_time_str()+" "+text<<std::endl;
#endif

}

/**
 * @brief Первичная разметка интерфейса.
 * Заполняет таблицу начальными значениями "Ожидание", чтобы пользователь
 * видел структуру программы сразу после запуска.
 */
void init_console(void)
{
    // Инициализируем строки таблицы
    update_console(0, "[Таймер] Ожидание...");
    update_console(1, "[Рабочий 1] Ожидание...");
    update_console(2, "[Рабочий 2] Ожидание...");
    update_console(3, "[Рабочий 3] Ожидание...");

}


///////////////////////////////////////////////////////////////////////////////////////////
