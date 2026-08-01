#pragma once
#include <QDialog>

class QLineEdit;

// Первый запуск: спрашиваем имя этого устройства и имя базы данных.
// Имя устройства видно собеседникам при синхронизации, имя базы должно
// совпадать у синхронизируемых устройств.
class FirstRunDialog : public QDialog {
    Q_OBJECT
public:
    FirstRunDialog(const QString& deviceName, const QString& dbName,
                   QWidget* parent = nullptr);
    QString deviceName() const;
    QString databaseName() const;

private:
    QLineEdit* device_;
    QLineEdit* db_;
};
