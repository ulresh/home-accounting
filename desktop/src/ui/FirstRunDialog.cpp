#include "FirstRunDialog.h"

#include <QLineEdit>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>

FirstRunDialog::FirstRunDialog(const QString& deviceName, const QString& dbName,
                               QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("ДомУчёт — первый запуск"));
    setMinimumWidth(420);

    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(
        tr("Имя устройства увидят те, с кем вы синхронизируетесь.\n"
           "Имя базы должно совпадать на всех устройствах одной семьи."), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* form = new QFormLayout();
    device_ = new QLineEdit(deviceName, this);
    device_->setPlaceholderText(tr("например, «Ноутбук Петра»"));
    db_ = new QLineEdit(dbName, this);
    form->addRow(tr("Имя устройства:"), device_);
    form->addRow(tr("Имя базы данных:"), db_);
    root->addLayout(form);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(box);

    device_->setFocus();
}

QString FirstRunDialog::deviceName() const { return device_->text().trimmed(); }
QString FirstRunDialog::databaseName() const { return db_->text().trimmed(); }
