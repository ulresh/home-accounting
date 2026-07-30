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

CatalogDialog::CatalogDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Редактор каталога"));
    resize(580, 430);

    cats_  = new QListWidget(this);
    cats_->setObjectName("cats");
    items_ = new QListWidget(this);
    items_->setObjectName("items");

    auto* addCat = new QPushButton(tr("+ категория"), this);
    addCat->setObjectName("addCat");
    auto* delCat = new QPushButton(tr("− категория"), this);
    delCat->setObjectName("delCat");
    auto* addIt  = new QPushButton(tr("+ элемент"), this);
    addIt->setObjectName("addItem");
    auto* delIt  = new QPushButton(tr("− элемент"), this);
    delIt->setObjectName("delItem");

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
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(bb);

    connect(addCat, &QPushButton::clicked, this, &CatalogDialog::addCategory);
    connect(delCat, &QPushButton::clicked, this, &CatalogDialog::removeCategory);
    connect(addIt,  &QPushButton::clicked, this, &CatalogDialog::addItem);
    connect(delIt,  &QPushButton::clicked, this, &CatalogDialog::removeItem);
    connect(cats_,  &QListWidget::currentRowChanged, this, [this]{ reloadItems(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);

    reloadCats();
}

QString CatalogDialog::currentCat() const {
    auto* it = cats_->currentItem();
    return it ? it->text() : QString();
}
QString CatalogDialog::currentItem() const {
    auto* it = items_->currentItem();
    return it ? it->text() : QString();
}

void CatalogDialog::reloadCats(const QString& select) {
    QString keep = select.isEmpty() ? currentCat() : select;
    cats_->clear();
    for (auto& [category, content] : store_.catalog())
        cats_->addItem(QString::fromStdString(category));
    if (cats_->count()) {
        auto found = cats_->findItems(keep, Qt::MatchExactly);
        cats_->setCurrentItem(found.isEmpty() ? cats_->item(0) : found.first());
    }
    reloadItems();
}

void CatalogDialog::reloadItems(const QString& select) {
    QString keep = select.isEmpty() ? currentItem() : select;
    items_->clear();
    auto cat = store_.catalog().find(currentCat().toStdString());
    if (cat == store_.catalog().end()) return;
    for (auto& [item, addtime] : cat->second.items)
        items_->addItem(QString::fromStdString(item));
    auto found = items_->findItems(keep, Qt::MatchExactly);
    if (!found.isEmpty()) items_->setCurrentItem(found.first());
}

void CatalogDialog::addCategory() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Новая категория"),
        tr("Название категории:"), QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (store_.catalog().contains(name.toStdString())) {
        QMessageBox::information(this, tr("Каталог"), tr("Такая категория уже есть."));
        return;
    }
    store_.upsertCatalog({name.toStdString(), {}});
    reloadCats(name);
}

void CatalogDialog::removeCategory() {
    QString cat = currentCat();
    if (cat.isEmpty()) return;
    auto r = QMessageBox::question(this, tr("Каталог"),
        tr("Удалить категорию «%1» со всем содержимым?").arg(cat));
    if (r != QMessageBox::Yes) return;
    store_.removeCatalogCategory(cat.toStdString());
    reloadCats();
}

void CatalogDialog::addItem() {
    QString cat = currentCat();
    if (cat.isEmpty()) {
        QMessageBox::information(this, tr("Каталог"), tr("Сначала выберите категорию."));
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Новый элемент"),
        tr("Наименование или название вложенной категории:"), QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    store_.upsertCatalog({cat.toStdString(), {name.toStdString()}});
    reloadItems(name);
}

void CatalogDialog::removeItem() {
    QString cat = currentCat(), item = currentItem();
    if (cat.isEmpty() || item.isEmpty()) return;
    store_.removeCatalogItem(cat.toStdString(), item.toStdString());
    reloadItems();
}
