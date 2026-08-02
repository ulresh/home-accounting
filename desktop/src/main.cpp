#include <QApplication>
#include <QMessageBox>
#include <QFont>
#include <QIcon>
#include <QSysInfo>
#include "model/Store.h"
#include "ui/MainWindow.h"
#include "ui/FirstRunDialog.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("home-accounting");
    QApplication::setOrganizationName("com.github.ulresh");
    QApplication::setWindowIcon(QIcon(":/icon.png"));

    ha::Store store;
    const bool firstRun = store.isFirstRun();   // до чтения/создания конфигурации

    // Спрашиваем ДО load(): база создаётся уже под введённым именем, иначе
    // рядом осталась бы никому не нужная «Основная».
    QString deviceName;
    if (firstRun) {
        FirstRunDialog dlg(QSysInfo::machineHostName(),
                           QString::fromStdString(store.database()));
        dlg.exec();
        deviceName = dlg.deviceName();
        store.setInitialDatabase(dlg.databaseName().toStdString());
    }

    try {
        store.load();
        store.ensureIdentity();   // ключ/сертификат + номер устройства
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "ДомУчёт",
            QString("Ошибка инициализации: %1").arg(e.what()));
        return 1;
    }

    if (firstRun) store.setDeviceName(deviceName.toStdString());

    if (store.fontSize() > 0) {                 // применить сохранённый размер шрифта
        QFont f = app.font();
        f.setPointSize(store.fontSize());
        app.setFont(f);
    }

    MainWindow w(store);
    w.show();
    return app.exec();
}
