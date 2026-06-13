#pragma once
#include <QDialog>

class QComboBox;
class QSpinBox;

namespace ha { class Store; }

// Настройки приложения: текущая база данных и размер шрифта.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ha::Store& store, QWidget* parent = nullptr);

signals:
    void databaseChanged();     // текущая база переключена/создана

private slots:
    void applyFont(int pt);
    void switchDb();

private:
    void reloadDbList();

    ha::Store& store_;
    QComboBox* dbCombo_;
    QSpinBox*  fontSpin_;
};
