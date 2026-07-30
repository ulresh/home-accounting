#pragma once
#include <QDialog>
#include <QString>

class QListWidget;

namespace ha { class Store; }

// Список людей («Кому»). Изменения пишутся в файл сразу.
class PeopleDialog : public QDialog {
    Q_OBJECT
public:
    explicit PeopleDialog(ha::Store& store, QWidget* parent = nullptr);

private slots:
    void addPerson();
    void removePerson();

private:
    void reload(const QString& select = {});

    ha::Store& store_;
    QListWidget* list_;
};
