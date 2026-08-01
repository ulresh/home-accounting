#include "DevicesDialog.h"
#include "../model/Store.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>
#include <QDialogButtonBox>

DevicesDialog::DevicesDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Устройства"));
    setMinimumSize(560, 320);

    auto* root = new QVBoxLayout(this);
    auto* hint = new QLabel(
        tr("Имя можно менять только у этого устройства — остальные называют "
           "себя сами.\n«Отключено» запрещает прямую синхронизацию с "
           "устройством."), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels(
        {tr("DN"), tr("Имя"), tr("Отключено"), tr("Ключ")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(table_, 1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);

    reload();
    connect(table_, &QTableWidget::itemChanged, this, &DevicesDialog::onItemChanged);
}

void DevicesDialog::reload() {
    filling_ = true;
    const auto& devs = store_.devices();
    table_->setRowCount((int)devs.size());
    int row = 0;
    for (const auto& d : devs) {
        bool self = d.no == store_.deviceNo();

        auto* dn = new QTableWidgetItem(
            self ? tr("%1 (это устройство)").arg(d.no) : QString::number(d.no));
        dn->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        dn->setData(Qt::UserRole, d.no);
        table_->setItem(row, 0, dn);

        auto* name = new QTableWidgetItem(QString::fromStdString(d.name));
        // Чужое имя задаёт его владелец: оно приходит при синхронизации.
        name->setFlags(self ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                               Qt::ItemIsEditable)
                            : (Qt::ItemIsEnabled | Qt::ItemIsSelectable));
        table_->setItem(row, 1, name);

        auto* off = new QTableWidgetItem();
        if (self) {   // сам с собой не синхронизируешься — флага нет
            off->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            off->setText(tr("—"));
        } else {
            off->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                          Qt::ItemIsUserCheckable);
            off->setCheckState(d.disabled ? Qt::Checked : Qt::Unchecked);
        }
        table_->setItem(row, 2, off);

        auto* key = new QTableWidgetItem(
            QString::fromStdString(d.pubkey).left(24) + "…");
        key->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(row, 3, key);
        ++row;
    }
    table_->resizeColumnsToContents();
    filling_ = false;
}

void DevicesDialog::onItemChanged(QTableWidgetItem* item) {
    if (filling_ || !item) return;
    auto* dnItem = table_->item(item->row(), 0);
    if (!dnItem) return;
    int no = dnItem->data(Qt::UserRole).toInt();

    if (item->column() == 1 && no == store_.deviceNo())
        store_.setDeviceName(item->text().trimmed().toStdString());
    else if (item->column() == 2)
        store_.setDeviceDisabled(no, item->checkState() == Qt::Checked);
}
