#include "Jsonl.h"
#include <fstream>
#include <random>
#include <chrono>
#include <iterator>
#include <boost/json.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace ha {

bool readValues(const fs::path& p,
                const std::function<void(const json::value&, std::string_view)>& onValue) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const char* data = content.data();
    std::size_t n = content.size();
    std::size_t pos = 0;
    json::stream_parser sp;

    while (pos < n) {
        // пропустить пробелы/переводы строк между значениями
        while (pos < n && static_cast<unsigned char>(data[pos]) <= ' ') ++pos;
        if (pos >= n) break;

        sp.reset();
        boost::system::error_code ec;
        std::size_t consumed = sp.write_some(data + pos, n - pos, ec);
        if (ec) break;                 // некорректный JSON — прекращаем
        std::size_t start = pos;
        pos += consumed;
        if (!sp.done()) break;         // неполное значение в конце файла

        json::value v = sp.release();
        std::size_t end = pos;
        while (end > start && static_cast<unsigned char>(data[end - 1]) <= ' ') --end;
        onValue(v, std::string_view(data + start, end - start));
    }
    return true;
}

void appendLine(const fs::path& p, const std::string& line) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::app);
    out << line << '\n';
    out.flush();
}

void writeAtomic(const fs::path& p, const std::string& content) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    fs::path tmp = p;
    tmp += ".tmp." + std::to_string(now) + "." + std::to_string(rd());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.flush();
    }
    fs::rename(tmp, p);
}

} // namespace ha
