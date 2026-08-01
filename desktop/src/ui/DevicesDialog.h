#pragma once
#include <QDialog>

class QTableWidget;
class QTableWidgetItem;

namespace ha { class Store; }

// Список устройств базы: имя своего устройства можно менять, остальным —
// ставить и снимать «Отключено» (запрет прямой синхронизации).
// Своё устройство отключить нельзя, а имя чужого задаёт его владелец —
// сюда оно приходит при синхронизации.
class DevicesDialog : public QDialog {
    Q_OBJECT
public:
    explicit DevicesDialog(ha::Store& store, QWidget* parent = nullptr);

private slots:
    void onItemChanged(QTableWidgetItem* item);

private:
    void reload();

    ha::Store& store_;
    QTableWidget* table_;
    bool filling_ = false;      // не реагировать на собственное заполнение
};
