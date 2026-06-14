#pragma once
#include <QDialog>
#include <vector>
#include "../model/Types.h"

class QListWidget;

namespace ha { class Store; }

// Редактор каталога: категории и входящие в них наименования или вложенные категории.
class CatalogDialog : public QDialog {
    Q_OBJECT
public:
    explicit CatalogDialog(ha::Store& store, QWidget* parent = nullptr);

private slots:
    void addCategory();
    void removeCategory();
    void addItem();
    void removeItem();
    void apply();

private:
    void reloadCats(int select = -1);
    void reloadItems();
    int  currentCat() const;

    ha::Store& store_;
    std::vector<ha::CatalogEntry> work_;   // рабочая копия
    QListWidget* cats_;
    QListWidget* items_;
};
