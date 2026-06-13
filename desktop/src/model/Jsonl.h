#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <functional>
#include <boost/json/value.hpp>

namespace ha {

// Прочитать файл как последовательность JSON-значений. Границы каждого значения
// определяет сам парсер (stream_parser), а не перевод строки.
// raw — исходный текст значения (без окружающих пробелов), действителен только
// в течение вызова колбэка.
bool readValues(const std::filesystem::path& p,
                const std::function<void(const boost::json::value&, std::string_view raw)>& onValue);

// Дозаписать одну строку (+\n) в конец файла, создав родительские папки.
// Файлы событий только дозаписываются — никогда не перезаписываются.
void appendLine(const std::filesystem::path& p, const std::string& line);

// Атомарно перезаписать файл целиком (запись во временный + rename).
void writeAtomic(const std::filesystem::path& p, const std::string& content);

} // namespace ha
