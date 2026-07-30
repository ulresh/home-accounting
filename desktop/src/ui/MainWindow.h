#pragma once
#include <QMainWindow>
#include <memory>
#include <vector>
#include "../model/Types.h"

class QTableWidget;
class QLineEdit;
class QLabel;
class QTimer;

namespace ha { class Store; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ha::Store& store, QWidget* parent = nullptr);

private slots:
    void refresh();
    void onAdd();
    void onEdit();
    void onDelete();
    void onSync();
    void onManagePeople();
    void onCatalog();
    void onSettings();

private:
    int selectedRow() const;

    ha::Store& store_;
    QTableWidget* table_;
    QLineEdit*    search_;
    QLabel*       dbLabel_;
    QTimer*       filterTimer_ = nullptr;
    // Модель отдаёт события как shared_ptr — те же объекты, что лежат в Store,
    // их и передаём обратно в deleteEvent/editEvent.
    std::vector<std::shared_ptr<ha::Event>> rows_;
};
