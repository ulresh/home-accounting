#pragma once
#include <filesystem>
#include <string>

namespace ha {

// getFilesDir() — корень данных приложения.
// Desktop: ~/.data/home-accounting
std::filesystem::path filesDir();

// Декада по году: 2026 -> "2020".
std::string decadeFolder(int year);

// Имя месячного файла по году/месяцу: 2026,6 -> "2606".
std::string monthFile(int year, int month);

// Путь к месячному файлу событий для базы db и момента редактирования (год,месяц).
std::filesystem::path eventFilePath(const std::filesystem::path& root,
                                    const std::string& db, int year, int month);

} // namespace ha
