#include <QApplication>
#include <QMessageBox>
#include <QFont>
#include "model/Store.h"
#include "ui/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("home-accounting");
    QApplication::setOrganizationName("com.github.ulresh");

    ha::Store store;
    try {
        store.load();
        store.ensureIdentity();   // ключ/сертификат + номер устройства
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "ДомУчёт",
            QString("Ошибка инициализации: %1").arg(e.what()));
        return 1;
    }

    if (store.fontSize() > 0) {                 // применить сохранённый размер шрифта
        QFont f = app.font();
        f.setPointSize(store.fontSize());
        app.setFont(f);
    }

    MainWindow w(store);
    w.show();
    return app.exec();
}
