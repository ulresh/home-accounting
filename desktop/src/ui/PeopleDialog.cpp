#include "PeopleDialog.h"
#include "WindowGeometry.h"
#include "../model/Store.h"

#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QMessageBox>

PeopleDialog::PeopleDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Люди"));
    resize(320, 380);

    list_ = new QListWidget(this);
    list_->setObjectName("people");

    auto* add = new QPushButton(tr("+ человек"), this);
    add->setObjectName("addPerson");
    auto* del = new QPushButton(tr("− человек"), this);
    del->setObjectName("delPerson");
    auto* btns = new QHBoxLayout();
    btns->addWidget(add); btns->addWidget(del);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Кому"), this));
    root->addWidget(list_, 1);
    root->addLayout(btns);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(bb);

    connect(add, &QPushButton::clicked, this, &PeopleDialog::addPerson);
    connect(del, &QPushButton::clicked, this, &PeopleDialog::removePerson);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);

    reload();
    ha::ui::rememberGeometry(this, "people");
}

void PeopleDialog::reload(const QString& select) {
    list_->clear();
    for (auto& [name, addtime] : store_.people())
        list_->addItem(QString::fromStdString(name));
    if (!select.isEmpty()) {
        auto found = list_->findItems(select, Qt::MatchExactly);
        if (!found.isEmpty()) list_->setCurrentItem(found.first());
    }
}

void PeopleDialog::addPerson() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Люди"),
        tr("Имя:"), QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    store_.addPerson(name.toStdString());
    reload(name);
}

void PeopleDialog::removePerson() {
    auto* it = list_->currentItem();
    if (!it) return;
    QString name = it->text();
    auto r = QMessageBox::question(this, tr("Люди"),
        tr("Удалить «%1» из списка?").arg(name));
    if (r != QMessageBox::Yes) return;
    store_.removePerson(name.toStdString());
    reload();
}
