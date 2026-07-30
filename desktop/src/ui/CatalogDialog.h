#pragma once
#include <QDialog>
#include <QString>

class QListWidget;

namespace ha { class Store; }

// Редактор каталога: категории и входящие в них наименования или вложенные
// категории. Изменения пишутся в файл сразу (отдельной команды «Сохранить» нет).
class CatalogDialog : public QDialog {
    Q_OBJECT
public:
    explicit CatalogDialog(ha::Store& store, QWidget* parent = nullptr);

private slots:
    void addCategory();
    void removeCategory();
    void addItem();
    void removeItem();

private:
    void reloadCats(const QString& select = {});
    void reloadItems(const QString& select = {});
    QString currentCat() const;
    QString currentItem() const;

    ha::Store& store_;
    QListWidget* cats_;
    QListWidget* items_;
};
