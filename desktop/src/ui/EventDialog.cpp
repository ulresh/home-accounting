#include "EventDialog.h"
#include "WindowGeometry.h"
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
    subject_->setObjectName("subject");
    subject_->setEditable(true);
    // Наполнить позициями каталога. Позиция может быть вложенной категорией —
    // такие в список наименований не берём, это группировка, а не товар.
    const auto& catalog = store_.catalog();
    std::set<QString> items;
    for (auto& [category, content] : catalog)
        for (auto& [item, addtime] : content.items)
            if (!catalog.contains(item))
                items.insert(QString::fromStdString(item));
    for (auto& it : items) subject_->addItem(it);
    subject_->setCurrentText("");

    cost_ = new QDoubleSpinBox(this);
    cost_->setObjectName("cost");
    cost_->setRange(0.0, 1e9);
    cost_->setDecimals(2);
    cost_->setGroupSeparatorShown(true);

    when_ = new QDateTimeEdit(QDateTime::currentDateTime(), this);
    when_->setObjectName("when");
    when_->setDisplayFormat("yyyy-MM-dd HH:mm");
    when_->setCalendarPopup(true);

    withTime_ = new QCheckBox(tr("учитывать время"), this);
    withTime_->setObjectName("withTime");
    withTime_->setChecked(true);

    people_ = new QComboBox(this);
    people_->setObjectName("people");
    people_->setEditable(true);
    people_->addItem("");
    for (auto& [name, addtime] : store_.people())
        people_->addItem(QString::fromStdString(name));

    volume_ = new QLineEdit(this);
    volume_->setObjectName("volume");
    volume_->setPlaceholderText(tr("напр. 2 кг"));

    comment_ = new QLineEdit(this);
    comment_->setObjectName("comment");
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
        people_->setCurrentText(QString::fromStdString(edit->people));
        volume_->setText(QString::fromStdString(edit->volume));
        comment_->setText(QString::fromStdString(edit->comment));
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

    ha::ui::rememberGeometry(this, "event");
}

QString EventDialog::eventDateTime() const {
    if (withTime_->isChecked())
        return when_->dateTime().toString("yyyy-MM-dd HH:mm");
    return when_->date().toString("yyyy-MM-dd");
}
QString EventDialog::subject() const { return subject_->currentText().trimmed(); }
double  EventDialog::cost() const { return cost_->value(); }

QString EventDialog::people() const  { return people_->currentText().trimmed(); }
QString EventDialog::volume() const  { return volume_->text().trimmed(); }
QString EventDialog::comment() const { return comment_->text().trimmed(); }
