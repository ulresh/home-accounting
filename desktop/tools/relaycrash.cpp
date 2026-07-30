// Минимальный воспроизводитель падения в aSendAllIncrement на трёхзвенной
// цепочке. НЕ в CMake — собирается вручную, см. комментарий внизу файла.
//
//   A ── B ── C : событие рождается у A, доезжает до C через B,
//   затем A его удаляет и снова синхронизируется с B.
//
// Падает на втором A↔B: SEGV в ~MonthSyncData() на выходе из корутины
// aSendAllIncrement (время жизни временных в кадре).
#include "../src/model/Store.h"
#include "../src/sync/SyncService.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace ha;
namespace fs = std::filesystem;

static auto YES = [](const std::string&) { return true; };

static void sync(Store& A, Store& B) {          // A — сервер, B — клиент
    SyncServer s(A);
    PairInfo info = s.listen();
    info.ip = "127.0.0.1";
    std::thread ts([&] { s.wait(YES); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SyncClient c(B);
    c.connect(info, YES);
    ts.join();
}

static std::shared_ptr<Event> findEvent(Store& s, const std::string& subj) {
    for (auto& e : s.events()) if (e->subject == subj) return e;
    return {};
}

int main(int argc, char** argv) {
    fs::path base = argc > 1 ? argv[1] : "./relaycrash-data";
    fs::remove_all(base);

    Store A(base / "a"); A.load(); A.ensureIdentity();
    Store B(base / "b"); B.load(); B.ensureIdentity();
    Store C(base / "c"); C.load(); C.ensureIdentity();
    // У C своя нумерация, чтобы DN пришлось переписывать.
    C.addDevice("PUBKEY-X");
    C.addDevice("PUBKEY-Y");

    A.addEvent("2026-08-01", "Молоко", 80, "", "1 л", "");

    std::cout << "1. sync A-B" << std::endl;  sync(A, B);
    std::cout << "2. sync B-C" << std::endl;  sync(B, C);
    std::cout << "3. delete on A" << std::endl;
    A.deleteEvent(findEvent(A, "Молоко"));
    std::cout << "4. sync A-B" << std::endl;  sync(A, B);   // <-- падает здесь
    std::cout << "5. sync B-C" << std::endl;  sync(B, C);
    std::cout << "готово, падения не было" << std::endl;
    return 0;
}

// Сборка и запуск на appbuild (из .../desktop):
//
//   g++ -std=c++20 -g -I src -I /usr/local/include -o relaycrash \
//       tools/relaycrash.cpp src/model/Store.cpp src/model/Jsonl.cpp \
//       src/model/Paths.cpp src/sync/Crypto.cpp src/sync/SyncService.cpp \
//       src/sync/QrCode.cpp \
//       -L/usr/local/lib -lboost_json -lssl -lcrypto -lpthread
//   LD_LIBRARY_PATH=/usr/local/lib timeout 60 ./relaycrash
//
// То же с диагностикой (добавить -fsanitize=address в команду выше) даёт стек
// с ~MonthSyncData() / ~Schema() и номером строки aSendAllIncrement.
