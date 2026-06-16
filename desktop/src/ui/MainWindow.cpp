#include "MainWindow.h"
#include "EventDialog.h"
#include "SyncDialog.h"
#include "SettingsDialog.h"
#include "CatalogDialog.h"
#include "../model/Store.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QTimer>
#include <QLabel>
#include <QToolBar>
#include <QAction>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QLocale>

MainWindow::MainWindow(ha::Store& store, QWidget* parent)
    : QMainWindow(parent), store_(store) {
    setWindowTitle("ДомУчёт");
    resize(900, 560);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    // Панель фильтра — одно поле (наименование / человек / категория)
    auto* filters = new QHBoxLayout();
    search_ = new QLineEdit(central);
    search_->setPlaceholderText(tr("Фильтр: наименование, кому или категория…"));
    search_->setClearButtonEnabled(true);
    filters->addWidget(search_, 1);
    root->addLayout(filters);

    // Дебаунс: фильтр пересчитывается фоном, быстрый набор нескольких букв
    // объединяется в одно обновление.
    filterTimer_ = new QTimer(this);
    filterTimer_->setSingleShot(true);
    filterTimer_->setInterval(150);
    connect(filterTimer_, &QTimer::timeout, this, &MainWindow::refresh);

    // Таблица
    table_ = new QTableWidget(central);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels(
        {tr("Дата"), tr("Категория"), tr("Наименование"), tr("Стоимость"), tr("Кому"), tr("Количество"), tr("Комментарий")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    root->addWidget(table_, 1);

    setCentralWidget(central);

    // Тулбар
    auto* tb = addToolBar(tr("Действия"));
    tb->setMovable(false);
    QAction* aAdd  = tb->addAction(tr("Добавить"));
    QAction* aEdit = tb->addAction(tr("Изменить"));
    QAction* aDel  = tb->addAction(tr("Удалить"));
    tb->addSeparator();
    QAction* aSync = tb->addAction(tr("Синхронизация"));
    QAction* aPpl  = tb->addAction(tr("Люди"));
    QAction* aCat  = tb->addAction(tr("Каталог"));
    QAction* aSet  = tb->addAction(tr("Настройки"));

    connect(aAdd,  &QAction::triggered, this, &MainWindow::onAdd);
    connect(aEdit, &QAction::triggered, this, &MainWindow::onEdit);
    connect(aDel,  &QAction::triggered, this, &MainWindow::onDelete);
    connect(aSync, &QAction::triggered, this, &MainWindow::onSync);
    connect(aPpl,  &QAction::triggered, this, &MainWindow::onManagePeople);
    connect(aCat,  &QAction::triggered, this, &MainWindow::onCatalog);
    connect(aSet,  &QAction::triggered, this, &MainWindow::onSettings);
    connect(table_, &QTableWidget::doubleClicked, this, &MainWindow::onEdit);

    connect(search_, &QLineEdit::textChanged, this, [this]{ filterTimer_->start(); });

    dbLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(dbLabel_);

    refresh();
}

int MainWindow::selectedRow() const {
    auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return -1;
    return sel.first().row();
}

void MainWindow::refresh() {
    dbLabel_->setText(tr("База: %1   Устройство №%2")
        .arg(QString::fromStdString(store_.database())).arg(store_.deviceNo()));

    const QString q = search_->text().trimmed();
    rows_ = store_.filter(q.toStdString());

    QLocale loc;
    table_->setRowCount((int)rows_.size());
    for (int i = 0; i < (int)rows_.size(); ++i) {
        const auto& e = rows_[i];
        auto set = [&](int col, const QString& s) {
            table_->setItem(i, col, new QTableWidgetItem(s));
        };
        set(0, QString::fromStdString(e.event_datetime));
        set(1, QString::fromStdString(store_.categoryOf(e.subject)));
        set(2, QString::fromStdString(e.subject));
        set(3, loc.toString(e.cost, 'f', (e.cost == (long long)e.cost) ? 0 : 2));
        set(4, e.people ? QString::fromStdString(*e.people) : QString());
        set(5, e.volume ? QString::fromStdString(*e.volume) : QString());
        set(6, e.comment ? QString::fromStdString(*e.comment) : QString());
    }
    statusBar()->showMessage(tr("Записей: %1").arg(rows_.size()));
}

void MainWindow::onAdd() {
    EventDialog dlg(store_, nullptr, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (dlg.subject().isEmpty()) {
        QMessageBox::warning(this, tr("Добавление"), tr("Укажите наименование."));
        return;
    }
    std::optional<std::string> ppl, vol, com;
    if (auto p = dlg.people()) ppl = p->toStdString();
    if (auto v = dlg.volume()) vol = v->toStdString();
    if (auto c = dlg.comment()) com = c->toStdString();
    store_.addEvent(dlg.eventDateTime().toStdString(), dlg.subject().toStdString(),
                    dlg.cost(), ppl, vol, com);
    refresh();
}

void MainWindow::onEdit() {
    int r = selectedRow();
    if (r < 0) { QMessageBox::information(this, tr("Изменение"), tr("Выберите запись.")); return; }
    ha::Event old = rows_[r];
    EventDialog dlg(store_, &old, this);
    if (dlg.exec() != QDialog::Accepted) return;
    std::optional<std::string> ppl, vol, com;
    if (auto p = dlg.people()) ppl = p->toStdString();
    if (auto v = dlg.volume()) vol = v->toStdString();
    if (auto c = dlg.comment()) com = c->toStdString();
    store_.editEvent(old, dlg.eventDateTime().toStdString(), dlg.subject().toStdString(),
                     dlg.cost(), ppl, vol, com);
    refresh();
}

void MainWindow::onDelete() {
    int r = selectedRow();
    if (r < 0) { QMessageBox::information(this, tr("Удаление"), tr("Выберите запись.")); return; }
    ha::Event e = rows_[r];
    auto resp = QMessageBox::question(this, tr("Удаление"),
        tr("Удалить запись «%1»?").arg(QString::fromStdString(e.subject)));
    if (resp != QMessageBox::Yes) return;
    store_.deleteEvent(e);
    refresh();
}

void MainWindow::onSync() {
    SyncDialog dlg(store_, this);
    dlg.exec();
    store_.load();   // перечитать после возможных изменений
    refresh();
}

void MainWindow::onManagePeople() {
    QStringList items;
    for (auto& p : store_.people()) items << QString::fromStdString(p);
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Люди"),
        tr("Текущие: %1\n\nДобавить человека:").arg(items.join(", ")),
        QLineEdit::Normal, "", &ok);
    if (ok && !name.trimmed().isEmpty()) {
        store_.addPerson(name.trimmed().toStdString());
        refresh();
    }
}

void MainWindow::onCatalog() {
    CatalogDialog dlg(store_, this);
    dlg.exec();
    refresh();
}

void MainWindow::onSettings() {
    SettingsDialog dlg(store_, this);
    connect(&dlg, &SettingsDialog::databaseChanged, this, &MainWindow::refresh);
    dlg.exec();
    refresh();
}
