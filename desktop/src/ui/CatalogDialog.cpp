#include "CatalogDialog.h"
#include "../model/Store.h"

#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <algorithm>

CatalogDialog::CatalogDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Редактор каталога"));
    resize(580, 430);
    work_ = store_.catalog();

    cats_  = new QListWidget(this);
    items_ = new QListWidget(this);

    auto* addCat = new QPushButton(tr("+ категория"), this);
    auto* delCat = new QPushButton(tr("− категория"), this);
    auto* addIt  = new QPushButton(tr("+ элемент"), this);
    auto* delIt  = new QPushButton(tr("− элемент"), this);

    auto* catBtns = new QHBoxLayout();
    catBtns->addWidget(addCat); catBtns->addWidget(delCat);
    auto* itemBtns = new QHBoxLayout();
    itemBtns->addWidget(addIt); itemBtns->addWidget(delIt);

    auto* left = new QVBoxLayout();
    left->addWidget(new QLabel(tr("Категории"), this));
    left->addWidget(cats_, 1);
    left->addLayout(catBtns);

    auto* right = new QVBoxLayout();
    right->addWidget(new QLabel(tr("Наименования / вложенные категории"), this));
    right->addWidget(items_, 1);
    right->addLayout(itemBtns);

    auto* cols = new QHBoxLayout();
    cols->addLayout(left, 1);
    cols->addLayout(right, 1);

    auto* root = new QVBoxLayout(this);
    root->addLayout(cols, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(bb);

    connect(addCat, &QPushButton::clicked, this, &CatalogDialog::addCategory);
    connect(delCat, &QPushButton::clicked, this, &CatalogDialog::removeCategory);
    connect(addIt,  &QPushButton::clicked, this, &CatalogDialog::addItem);
    connect(delIt,  &QPushButton::clicked, this, &CatalogDialog::removeItem);
    connect(cats_,  &QListWidget::currentRowChanged, this, [this]{ reloadItems(); });
    connect(bb, &QDialogButtonBox::accepted, this, &CatalogDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reloadCats(work_.empty() ? -1 : 0);
}

int CatalogDialog::currentCat() const { return cats_->currentRow(); }

void CatalogDialog::reloadCats(int select) {
    cats_->clear();
    for (auto& c : work_) cats_->addItem(QString::fromStdString(c.category));
    if (select >= 0 && select < (int)work_.size()) cats_->setCurrentRow(select);
    reloadItems();
}

void CatalogDialog::reloadItems() {
    items_->clear();
    int r = currentCat();
    if (r < 0 || r >= (int)work_.size()) return;
    for (auto& it : work_[r].items) items_->addItem(QString::fromStdString(it));
}

void CatalogDialog::addCategory() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Новая категория"),
        tr("Название категории:"), QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    for (auto& c : work_)
        if (QString::fromStdString(c.category) == name) {
            QMessageBox::information(this, tr("Каталог"), tr("Такая категория уже есть."));
            return;
        }
    work_.push_back({name.toStdString(), {}});
    reloadCats((int)work_.size() - 1);
}

void CatalogDialog::removeCategory() {
    int r = currentCat();
    if (r < 0 || r >= (int)work_.size()) return;
    work_.erase(work_.begin() + r);
    reloadCats(std::min(r, (int)work_.size() - 1));
}

void CatalogDialog::addItem() {
    int r = currentCat();
    if (r < 0 || r >= (int)work_.size()) {
        QMessageBox::information(this, tr("Каталог"), tr("Сначала выберите категорию."));
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Новый элемент"),
        tr("Наименование или название вложенной категории:"), QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    auto& items = work_[r].items;
    if (std::find(items.begin(), items.end(), name.toStdString()) == items.end())
        items.push_back(name.toStdString());
    reloadItems();
}

void CatalogDialog::removeItem() {
    int r = currentCat();
    if (r < 0 || r >= (int)work_.size()) return;
    int ir = items_->currentRow();
    if (ir < 0 || ir >= (int)work_[r].items.size()) return;
    work_[r].items.erase(work_[r].items.begin() + ir);
    reloadItems();
}

void CatalogDialog::apply() {
    store_.replaceCatalog(work_);
    accept();
}
