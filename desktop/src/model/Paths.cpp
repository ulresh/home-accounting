#include "Paths.h"
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;

namespace ha {

fs::path filesDir() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::current_path();
    return base / ".data" / "home-accounting";
}

std::string decadeFolder(int year) {
    int d = (year / 10) * 10;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", d);
    return buf;
}

std::string monthFile(int year, int month) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d%02d", year % 100, month);
    return buf;
}

fs::path eventFilePath(const fs::path& root, const std::string& db, int year, int month) {
    return root / db / decadeFolder(year) / (monthFile(year, month) + ".jsonl");
}

} // namespace ha
