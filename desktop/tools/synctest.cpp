// Проверка синхронизации: (1) отмена ожидания не виснет;
// (2) DN-map устраняет потерю данных при одинаковых стартовых DN;
// (3) смену собственного DR делает только одно устройство (без данных).
#include "../src/model/Store.h"
#include "../src/sync/SyncService.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <filesystem>

using namespace ha;
namespace fs = std::filesystem;

static auto YES = [](const std::string&) { return true; };

// Выполнить синхронизацию: A — сервер, B — клиент. Вернуть пару результатов.
static std::pair<SyncResult, SyncResult> sync(Store& A, Store& B) {
    SyncServer s(A);
    PairInfo info = s.listen();
    info.ip = "127.0.0.1";
    SyncResult ra;
    std::thread ts([&] { ra = s.wait(YES); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SyncClient c(B);
    SyncResult rb = c.connect(info, YES);
    ts.join();
    return {ra, rb};
}

static void dump(const char* tag, Store& s) {
    std::cout << "  " << tag << " (DN=" << s.deviceNo() << ", событий "
              << s.events().size() << ", устройств " << s.devices().size() << "): ";
    for (auto& e : s.events()) std::cout << e.subject << "/" << e.dev_no << " ";
    std::cout << "\n";
}

int main() {
    alarm(30);

    // (1) отмена ожидания
    {
        fs::remove_all("/tmp/hs0");
        Store A("/tmp/hs0/.data/home-accounting"); A.load(); A.ensureIdentity();
        SyncServer s(A); s.listen();
        SyncResult r;
        std::thread t([&] { r = s.wait(YES); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        s.cancel(); t.join();
        std::cout << "[1] cancel: join OK, error=\"" << r.error << "\"\n";
    }

    // (2) оба устройства «из коробки» (DN=1), у обоих есть данные -> map спасает
    {
        fs::remove_all("/tmp/hsA"); fs::remove_all("/tmp/hsB");
        Store A("/tmp/hsA/.data/home-accounting"); A.load(); A.ensureIdentity();
        Store B("/tmp/hsB/.data/home-accounting"); B.load(); B.ensureIdentity();
        A.addEvent("2026-06-01", "Вишня", 100, std::nullopt, std::nullopt);
        A.addEvent("2026-06-02", "Стол", 8200, std::nullopt, std::nullopt);
        B.addEvent("2026-06-03", "Хлеб", 50, std::nullopt, std::nullopt);
        std::cout << "[2] both-have-data, start DN A=" << A.deviceNo() << " B=" << B.deviceNo() << "\n";
        auto [ra, rb] = sync(A, B);
        std::cout << "  server(A): передано=" << ra.sent << " принято=" << ra.received << "\n";
        std::cout << "  client(B): передано=" << rb.sent << " принято=" << rb.received << "\n";
        Store A2("/tmp/hsA/.data/home-accounting"); A2.load();
        Store B2("/tmp/hsB/.data/home-accounting"); B2.load();
        dump("A2", A2); dump("B2", B2);
    }

    // (3) клиент без данных (DN=1) — должен сменить свой DN, сервер сохранить
    {
        fs::remove_all("/tmp/hsC"); fs::remove_all("/tmp/hsD");
        Store A("/tmp/hsC/.data/home-accounting"); A.load(); A.ensureIdentity();
        Store B("/tmp/hsD/.data/home-accounting"); B.load(); B.ensureIdentity();
        A.addPerson("Мария");
        A.addEvent("2026-06-01", "Вишня", 100, std::string("Мария"), std::nullopt);
        // B без данных
        std::cout << "[3] client-no-data, start DN A=" << A.deviceNo() << " B=" << B.deviceNo() << "\n";
        auto [ra, rb] = sync(A, B);
        std::cout << "  server(A): передано=" << ra.sent << " принято=" << ra.received
                  << "  итог DN A=" << A.deviceNo() << "\n";
        std::cout << "  client(B): передано=" << rb.sent << " принято=" << rb.received
                  << "  итог DN B=" << B.deviceNo() << "\n";
        Store A2("/tmp/hsC/.data/home-accounting"); A2.load();
        Store B2("/tmp/hsD/.data/home-accounting"); B2.load();
        dump("A2", A2); dump("B2", B2);
        std::cout << "  B знает людей: "; for (auto& p : B2.people()) std::cout << p << " "; std::cout << "\n";
    }
    return 0;
}
