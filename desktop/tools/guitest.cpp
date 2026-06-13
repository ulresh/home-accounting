// Offscreen GUI-тест сценария зависания: открыть синхронизацию, запустить
// ожидание соединения («Старт»), затем закрыть диалог. Закрытие должно
// прервать ожидание и не приводить к зависанию.
#include <QApplication>
#include <QPushButton>
#include <QTimer>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include "model/Store.h"
#include "ui/SyncDialog.h"

int main(int argc, char** argv) {
    alarm(25);   // сторож: если зависнет — процесс будет убит
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::filesystem::remove_all("/tmp/haguitest");
    ha::Store store("/tmp/haguitest/.data/home-accounting");
    store.load();
    store.ensureIdentity();

    auto* dlg = new SyncDialog(store);
    dlg->show();

    QTimer::singleShot(200, [&] {
        for (auto* b : dlg->findChildren<QPushButton*>())
            if (b->text() == "Старт") { printf("click Старт -> ожидание соединения\n"); fflush(stdout); b->click(); break; }
    });

    QTimer::singleShot(900, [&] {
        printf("закрываю окно синхронизации...\n"); fflush(stdout);
        dlg->close();
        delete dlg;                       // деструктор: cancel() + join()
        printf("OK: диалог уничтожен, зависания нет\n"); fflush(stdout);
        app.quit();
    });

    int rc = app.exec();
    printf("приложение завершилось rc=%d\n", rc);
    return 0;
}
