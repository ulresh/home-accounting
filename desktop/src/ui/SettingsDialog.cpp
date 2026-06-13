#include "SettingsDialog.h"
#include "../model/Store.h"

#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QApplication>
#include <QFont>

SettingsDialog::SettingsDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Настройки"));
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);

    // --- База данных ---
    auto* gDb = new QGroupBox(tr("База данных"), this);
    auto* lDb = new QVBoxLayout(gDb);
    dbCombo_ = new QComboBox(gDb);
    dbCombo_->setEditable(true);
    reloadDbList();
    auto* dbRow = new QHBoxLayout();
    dbRow->addWidget(dbCombo_, 1);
    auto* dbBtn = new QPushButton(tr("Переключить / создать"), gDb);
    dbRow->addWidget(dbBtn);
    lDb->addWidget(new QLabel(tr("Текущая база переключается здесь:"), gDb));
    lDb->addLayout(dbRow);
    root->addWidget(gDb);

    // --- Шрифт ---
    auto* gFont = new QGroupBox(tr("Шрифт приложения"), this);
    auto* fForm = new QFormLayout(gFont);
    fontSpin_ = new QSpinBox(gFont);
    fontSpin_->setRange(7, 32);
    fontSpin_->setSuffix(tr(" pt"));
    int cur = store_.fontSize();
    if (cur <= 0) cur = qApp->font().pointSize();
    if (cur <= 0) cur = 10;
    fontSpin_->setValue(cur);
    fForm->addRow(tr("Размер:"), fontSpin_);
    root->addWidget(gFont);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(bb);

    connect(dbBtn, &QPushButton::clicked, this, &SettingsDialog::switchDb);
    connect(fontSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::applyFont);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);
}

void SettingsDialog::reloadDbList() {
    QString cur = QString::fromStdString(store_.database());
    dbCombo_->clear();
    for (auto& d : store_.databases()) dbCombo_->addItem(QString::fromStdString(d));
    int idx = dbCombo_->findText(cur);
    dbCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    dbCombo_->setCurrentText(cur);
}

void SettingsDialog::applyFont(int pt) {
    QFont f = qApp->font();
    f.setPointSize(pt);
    qApp->setFont(f);          // применяется ко всему приложению немедленно
    store_.setFontSize(pt);    // сохраняется в config.json
}

void SettingsDialog::switchDb() {
    QString name = dbCombo_->currentText().trimmed();
    if (name.isEmpty()) return;
    if (name == QString::fromStdString(store_.database())) return;
    store_.switchDatabase(name.toStdString(), true);
    reloadDbList();
    emit databaseChanged();
}
