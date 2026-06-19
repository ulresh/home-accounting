#pragma once
#include <filesystem>
#include <string>
#include <functional>
#include <boost/json/value.hpp>

namespace ha {

// Прочитать файл как последовательность JSON-значений. Файл читается БЛОКАМИ
// фиксированного размера, каждый блок сразу передаётся инкрементному парсеру —
// целиком в память файл не загружается. Границы значений определяет парсер.
bool readValues(const std::filesystem::path& p,
                const std::function<void(const boost::json::value&)>& onValue);

// Дозаписать одну строку (+\n) в конец файла, создав родительские папки.
void appendLine(const std::filesystem::path& p, const std::string& line);

// Атомарно перезаписать файл целиком (запись во временный + rename).
void writeAtomic(const std::filesystem::path& p, const std::string& content);

} // namespace ha
