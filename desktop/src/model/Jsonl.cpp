#include "Jsonl.h"
#include <fstream>
#include <random>
#include <chrono>
#include <vector>
#include <boost/json.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace ha {

bool readValues(const fs::path& p,
                const std::function<void(const json::value&)>& onValue) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;

    json::stream_parser sp;
    bool atStart = true;                  // мы на границе перед новым значением
    std::vector<char> block(64 * 1024);

    while (in) {
        in.read(block.data(), (std::streamsize)block.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        const char* p2 = block.data();
        std::size_t rem = (std::size_t)got;
        while (rem > 0) {
            if (atStart) {                // пропустить пробелы/переводы строк между значениями
                while (rem > 0 && (unsigned char)*p2 <= ' ') { ++p2; --rem; }
                if (rem == 0) break;
                atStart = false;
            }
            boost::system::error_code ec;
            std::size_t consumed = sp.write_some(p2, rem, ec);
            p2 += consumed; rem -= consumed;
            if (ec) return true;          // некорректный JSON — прекращаем
            if (sp.done()) { onValue(sp.release()); sp.reset(); atStart = true; }
            else break;                   // значение не закончилось — нужен следующий блок
        }
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
