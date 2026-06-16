#include "EventDialog.h"
#include "../model/Store.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDateTime>
#include <set>

EventDialog::EventDialog(ha::Store& store, const ha::Event* edit, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(edit ? tr("Изменить запись") : tr("Новая запись"));
    setMinimumWidth(360);

    subject_ = new QComboBox(this);
    subject_->setEditable(true);
    // наполнить позициями каталога
    std::set<QString> items;
    for (auto& c : store_.catalog())
        for (auto& it : c.items)
            items.insert(QString::fromStdString(it));
    for (auto& it : items) subject_->addItem(it);
    subject_->setCurrentText("");

    cost_ = new QDoubleSpinBox(this);
    cost_->setRange(0.0, 1e9);
    cost_->setDecimals(2);
    cost_->setGroupSeparatorShown(true);

    when_ = new QDateTimeEdit(QDateTime::currentDateTime(), this);
    when_->setDisplayFormat("yyyy-MM-dd HH:mm");
    when_->setCalendarPopup(true);

    withTime_ = new QCheckBox(tr("учитывать время"), this);
    withTime_->setChecked(true);

    people_ = new QComboBox(this);
    people_->setEditable(true);
    people_->addItem("");
    for (auto& p : store_.people()) people_->addItem(QString::fromStdString(p));

    volume_ = new QLineEdit(this);
    volume_->setPlaceholderText(tr("напр. 2 кг"));

    comment_ = new QLineEdit(this);
    comment_->setPlaceholderText(tr("необязательно"));

    if (edit) {
        subject_->setCurrentText(QString::fromStdString(edit->subject));
        cost_->setValue(edit->cost);
        QString dt = QString::fromStdString(edit->event_datetime);
        if (dt.size() <= 10) {
            withTime_->setChecked(false);
            when_->setDateTime(QDateTime::fromString(dt, "yyyy-MM-dd"));
        } else {
            when_->setDateTime(QDateTime::fromString(dt, "yyyy-MM-dd HH:mm"));
        }
        if (edit->people) people_->setCurrentText(QString::fromStdString(*edit->people));
        if (edit->volume) volume_->setText(QString::fromStdString(*edit->volume));
        if (edit->comment) comment_->setText(QString::fromStdString(*edit->comment));
    }

    auto* form = new QFormLayout(this);
    form->addRow(tr("Наименование:"), subject_);
    form->addRow(tr("Стоимость:"), cost_);
    form->addRow(tr("Дата:"), when_);
    form->addRow("", withTime_);
    form->addRow(tr("Кому:"), people_);
    form->addRow(tr("Количество:"), volume_);
    form->addRow(tr("Комментарий:"), comment_);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString EventDialog::eventDateTime() const {
    if (withTime_->isChecked())
        return when_->dateTime().toString("yyyy-MM-dd HH:mm");
    return when_->date().toString("yyyy-MM-dd");
}
QString EventDialog::subject() const { return subject_->currentText().trimmed(); }
double  EventDialog::cost() const { return cost_->value(); }

std::optional<QString> EventDialog::people() const {
    QString p = people_->currentText().trimmed();
    if (p.isEmpty()) return std::nullopt;
    return p;
}
std::optional<QString> EventDialog::volume() const {
    QString v = volume_->text().trimmed();
    if (v.isEmpty()) return std::nullopt;
    return v;
}
std::optional<QString> EventDialog::comment() const {
    QString c = comment_->text().trimmed();
    if (c.isEmpty()) return std::nullopt;
    return c;
}
