// Offscreen-тесты интерфейса против переработанных model/sync:
//   1) таблица главного окна: колонки, категория, фильтр;
//   2) добавление / правка / удаление записи через диалоги (shared_ptr-события);
//   3) редакторы каталога и людей — запись сразу в файл, без «Сохранить»;
//   4) прежний сценарий зависания: закрытие окна синхронизации во время
//      ожидания подключения должно прерывать его;
//   5) первый запуск (имя устройства и базы) и список устройств.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unistd.h>

#include "model/Store.h"
#include "ui/MainWindow.h"
#include "ui/CatalogDialog.h"
#include "ui/PeopleDialog.h"
#include "ui/SyncDialog.h"
#include "ui/DevicesDialog.h"
#include "ui/FirstRunDialog.h"

namespace fs = std::filesystem;

static int g_fail = 0, g_total = 0;
static void check(bool ok, const std::string& msg) {
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  [FAIL] %s\n", msg.c_str()); }
    else      std::printf("  [ ok ] %s\n", msg.c_str());
    std::fflush(stdout);
}

// Прокрутить очередь событий заданное время (таймеры, отложенные вызовы).
static void spin(int ms) {
    QElapsedTimer t; t.start();
    do { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
    while (t.elapsed() < ms);
}

// Действие над модальным окном, которое вот-вот откроется (exec() запускает
// собственный цикл событий, поэтому одноразовый таймер в нём и сработает).
static void onNextModal(std::function<void(QWidget*)> fn, int ms = 80) {
    QTimer::singleShot(ms, [fn]() {
        QWidget* w = QApplication::activeModalWidget();
        if (!w) { std::printf("  [!!] модальное окно не появилось\n"); return; }
        fn(w);
    });
}

static void answerYes(int ms = 80) {
    onNextModal([](QWidget* w) {
        auto* mb = qobject_cast<QMessageBox*>(w);
        if (!mb) { std::printf("  [!!] ожидался QMessageBox\n"); return; }
        if (auto* b = mb->button(QMessageBox::Yes)) b->click();
        else mb->accept();
    }, ms);
}

static void typeIntoInputDialog(const QString& text, int ms = 80) {
    onNextModal([text](QWidget* w) {
        auto* d = qobject_cast<QInputDialog*>(w);
        if (!d) { std::printf("  [!!] ожидался QInputDialog\n"); return; }
        d->setTextValue(text);
        d->accept();
    }, ms);
}

static int rowOfSubject(QTableWidget* t, const QString& subject) {
    for (int i = 0; i < t->rowCount(); ++i)
        if (t->item(i, 2) && t->item(i, 2)->text() == subject) return i;
    return -1;
}

static std::shared_ptr<ha::Event> findEvent(ha::Store& s, const std::string& subject) {
    for (auto& e : s.events()) if (e->subject == subject) return e;
    return {};
}

// ---------------------------------------------------------------- 1, 2 ----
static void testMainWindow(const fs::path& root) {
    std::printf("== 1. главное окно: таблица и фильтр ==\n");
    ha::Store store(root);
    store.load();
    store.ensureIdentity();
    store.addPerson("Мария");
    store.upsertCatalog({"Бакалея", {"Сахар", "Соль"}});
    store.addEvent("2026-06-10", "Кофе", 250, "", "1 шт", "из кофейни");
    store.addEvent("2026-06-11", "Сахар", 60, "Мария", "", "");

    MainWindow w(store);
    w.show();
    auto* table  = w.findChild<QTableWidget*>("events");
    auto* search = w.findChild<QLineEdit*>("search");
    check(table && search, "главное окно построено");
    if (!table || !search) return;

    check(table->rowCount() == 2, "в таблице 2 записи");
    int rCoffee = rowOfSubject(table, "Кофе");
    int rSugar  = rowOfSubject(table, "Сахар");
    check(rCoffee >= 0 && rSugar >= 0, "обе записи видны в таблице");
    if (rCoffee >= 0) {
        check(table->item(rCoffee, 0)->text() == "2026-06-10", "колонка «Дата»");
        check(table->item(rCoffee, 3)->text() == "250", "колонка «Стоимость» без копеек");
        check(table->item(rCoffee, 5)->text() == "1 шт", "колонка «Количество»");
        check(table->item(rCoffee, 6)->text() == "из кофейни", "колонка «Комментарий»");
        check(table->item(rCoffee, 1)->text().isEmpty(), "без категории — пусто");
    }
    if (rSugar >= 0) {
        check(table->item(rSugar, 1)->text() == "Бакалея", "категория подставлена из каталога");
        check(table->item(rSugar, 4)->text() == "Мария", "колонка «Кому»");
    }

    // Фильтр по названию категории — через членов категории.
    search->setText("Бакалея");
    spin(400);                       // дебаунс 150 мс
    check(table->rowCount() == 1 && rowOfSubject(table, "Сахар") == 0,
          "фильтр по категории оставил только «Сахар»");
    search->setText("кофе");         // регистронезависимо, кириллица
    spin(400);
    check(table->rowCount() == 1 && rowOfSubject(table, "Кофе") == 0,
          "фильтр по наименованию без учёта регистра");
    search->setText("");
    spin(400);
    check(table->rowCount() == 2, "пустой фильтр возвращает все записи");

    std::printf("== 2. добавление / правка / удаление через диалоги ==\n");
    onNextModal([](QWidget* dw) {
        auto* d = qobject_cast<QDialog*>(dw);
        d->findChild<QComboBox*>("subject")->setCurrentText("Молоко");
        d->findChild<QDoubleSpinBox*>("cost")->setValue(85.5);
        d->findChild<QCheckBox*>("withTime")->setChecked(false);
        d->findChild<QDateTimeEdit*>("when")->setDateTime(
            QDateTime::fromString("2026-06-15 09:00", "yyyy-MM-dd HH:mm"));
        d->findChild<QComboBox*>("people")->setCurrentText("Мария");
        d->findChild<QLineEdit*>("volume")->setText("1 л");
        d->findChild<QLineEdit*>("comment")->setText("к завтраку");
        d->accept();
    });
    QMetaObject::invokeMethod(&w, "onAdd");
    spin(100);

    auto milk = findEvent(store, "Молоко");
    check(milk != nullptr, "запись добавлена в модель");
    if (milk) {
        check(milk->event_datetime == "2026-06-15", "дата без времени (флажок снят)");
        check(milk->cost == 85.5, "дробная стоимость сохранена");
        check(milk->people == "Мария", "поле «Кому» записано строкой");
        check(milk->volume == "1 л" && milk->comment == "к завтраку",
              "количество и комментарий записаны");
    }
    check(table->rowCount() == 3 && rowOfSubject(table, "Молоко") >= 0,
          "таблица обновилась после добавления");

    // Правка: выбираем строку, меняем наименование и цену.
    table->selectRow(rowOfSubject(table, "Молоко"));
    onNextModal([](QWidget* dw) {
        auto* d = qobject_cast<QDialog*>(dw);
        auto* subj = d->findChild<QComboBox*>("subject");
        auto* cost = d->findChild<QDoubleSpinBox*>("cost");
        check(subj->currentText() == "Молоко", "диалог правки открыт с прежним наименованием");
        check(cost->value() == 85.5, "диалог правки открыт с прежней ценой");
        check(d->findChild<QLineEdit*>("comment")->text() == "к завтраку",
              "диалог правки открыт с прежним комментарием");
        check(d->findChild<QCheckBox*>("withTime")->isChecked() == false,
              "дата без времени распознана диалогом");
        subj->setCurrentText("Молоко 2.5%");
        cost->setValue(90);
        d->accept();
    });
    QMetaObject::invokeMethod(&w, "onEdit");
    spin(100);

    check(findEvent(store, "Молоко") == nullptr, "старая версия записи убрана");
    auto milk2 = findEvent(store, "Молоко 2.5%");
    check(milk2 && milk2->cost == 90, "новая версия записи с новой ценой");
    check(milk2 && milk2->volume == "1 л", "неизменённые поля сохранились при правке");
    check(table->rowCount() == 3, "после правки записей по-прежнему 3");

    // Удаление с подтверждением.
    table->selectRow(rowOfSubject(table, "Кофе"));
    answerYes();
    QMetaObject::invokeMethod(&w, "onDelete");
    spin(100);
    check(findEvent(store, "Кофе") == nullptr, "запись удалена из модели");
    check(table->rowCount() == 2 && rowOfSubject(table, "Кофе") < 0,
          "таблица обновилась после удаления");

    // Отказ от удаления ничего не меняет.
    table->selectRow(rowOfSubject(table, "Сахар"));
    onNextModal([](QWidget* dw) {
        auto* mb = qobject_cast<QMessageBox*>(dw);
        if (auto* b = mb->button(QMessageBox::No)) b->click(); else mb->reject();
    });
    QMetaObject::invokeMethod(&w, "onDelete");
    spin(100);
    check(findEvent(store, "Сахар") != nullptr, "отказ от удаления сохраняет запись");

    // Всё записано сразу — перечитываем базу с диска.
    ha::Store re(root);
    re.load();
    check(findEvent(re, "Молоко 2.5%") != nullptr, "правка видна после перезагрузки");
    check(findEvent(re, "Кофе") == nullptr, "удаление видно после перезагрузки");
    check(findEvent(re, "Сахар") != nullptr, "оставшаяся запись видна после перезагрузки");
    auto m = findEvent(re, "Молоко 2.5%");
    check(m && m->people == "Мария" && m->volume == "1 л",
          "поля записи прочитаны с диска");
}

// ------------------------------------------------------------------- 3 ----
static void testCatalogAndPeople(const fs::path& root) {
    std::printf("== 3. редакторы каталога и людей ==\n");
    ha::Store store(root);
    store.load();
    store.ensureIdentity();
    store.upsertCatalog({"Бакалея", {"Сахар", "Соль"}});

    {
        CatalogDialog dlg(store);
        dlg.show();
        auto* cats  = dlg.findChild<QListWidget*>("cats");
        auto* items = dlg.findChild<QListWidget*>("items");
        check(cats && items, "редактор каталога построен");
        if (!cats || !items) return;
        check(cats->count() == 1 && cats->item(0)->text() == "Бакалея",
              "категория показана");
        check(items->count() == 2, "позиции категории показаны");

        typeIntoInputDialog("Овощи");
        dlg.findChild<QPushButton*>("addCat")->click();
        spin(100);
        check(store.catalog().contains("Овощи"), "категория добавлена в модель");
        check(cats->count() == 2, "категория появилась в списке");
        check(cats->currentItem() && cats->currentItem()->text() == "Овощи",
              "новая категория выбрана");

        typeIntoInputDialog("Помидоры");
        dlg.findChild<QPushButton*>("addItem")->click();
        spin(100);
        auto veg = store.catalog().find("Овощи");
        check(veg != store.catalog().end() && veg->second.items.contains("Помидоры"),
              "позиция добавлена в модель");
        check(items->count() == 1 && items->item(0)->text() == "Помидоры",
              "позиция появилась в списке");

        // Удаление позиции.
        items->setCurrentRow(0);
        dlg.findChild<QPushButton*>("delItem")->click();
        spin(100);
        veg = store.catalog().find("Овощи");
        check(veg != store.catalog().end() && !veg->second.items.contains("Помидоры"),
              "позиция удалена из модели");
        check(veg != store.catalog().end() && veg->second.deleted.contains("Помидоры"),
              "позиция перенесена в раздел удалённых (для слияния)");
        check(items->count() == 0, "список позиций опустел");

        // Удаление категории с подтверждением.
        answerYes();
        dlg.findChild<QPushButton*>("delCat")->click();
        spin(100);
        check(!store.catalog().contains("Овощи"), "категория удалена из модели");
        check(cats->count() == 1, "в списке осталась одна категория");
    }

    {
        PeopleDialog dlg(store);
        dlg.show();
        auto* list = dlg.findChild<QListWidget*>("people");
        check(list && list->count() == 0, "редактор людей построен, список пуст");
        if (!list) return;

        typeIntoInputDialog("Мария");
        dlg.findChild<QPushButton*>("addPerson")->click();
        spin(100);
        typeIntoInputDialog("Пётр");
        dlg.findChild<QPushButton*>("addPerson")->click();
        spin(100);
        check(store.people().size() == 2, "оба человека добавлены в модель");
        check(list->count() == 2, "оба человека показаны в списке");

        auto found = list->findItems("Пётр", Qt::MatchExactly);
        check(!found.isEmpty(), "«Пётр» есть в списке");
        if (!found.isEmpty()) list->setCurrentItem(found.first());
        answerYes();
        dlg.findChild<QPushButton*>("delPerson")->click();
        spin(100);
        check(!store.people().contains("Пётр"), "человек удалён из модели");
        check(store.people().contains("Мария"), "остальные не тронуты");
        check(list->count() == 1, "список обновился");
    }

    // Перезагрузка: каталог и люди читаются с диска без ошибок.
    ha::Store re(root);
    bool loaded = true;
    try { re.load(); }
    catch (const std::exception& e) {
        loaded = false;
        std::printf("  [!!] исключение при загрузке: %s\n", e.what());
    }
    check(loaded, "база перечитана без исключений");
    if (!loaded) return;
    check(re.catalog().size() == 1 && re.catalog().contains("Бакалея"),
          "каталог после перезагрузки: осталась «Бакалея»");
    auto bak = re.catalog().find("Бакалея");
    check(bak != re.catalog().end() && bak->second.items.size() == 2,
          "позиции «Бакалеи» прочитаны с диска");
    check(re.people().size() == 1 && re.people().contains("Мария"),
          "люди после перезагрузки: осталась «Мария»");
}

// ------------------------------------------------------------------- 4 ----
static void testSyncCancel(const fs::path& root) {
    std::printf("== 4. отмена синхронизации закрытием окна ==\n");
    ha::Store store(root);
    store.load();
    store.ensureIdentity();

    auto* dlg = new SyncDialog(store);
    dlg->show();

    QEventLoop loop;
    bool clicked = false, closed = false;
    QTimer::singleShot(200, [&] {
        for (auto* b : dlg->findChildren<QPushButton*>())
            if (b->text() == "Старт") { b->click(); clicked = true; break; }
    });
    QTimer::singleShot(900, [&] {
        dlg->close();
        delete dlg;                   // деструктор: cancel() + join()
        closed = true;
        loop.quit();
    });
    loop.exec();
    check(clicked, "кнопка «Старт» нажата, сервер слушает");
    check(closed, "диалог закрыт и уничтожен без зависания");
}

// ------------------------------------------------------------------- 5 ----
static void testFirstRunAndDevices(const fs::path& root) {
    std::printf("== 5. первый запуск и список устройств ==\n");
    {   // диалог первого запуска отдаёт то, что ввёл пользователь
        FirstRunDialog dlg("host-1", "Основная");
        auto edits = dlg.findChildren<QLineEdit*>();
        check(edits.size() == 2, "в диалоге два поля: устройство и база");
        check(dlg.deviceName() == "host-1" && dlg.databaseName() == "Основная",
              "поля предзаполнены именем машины и текущей базой");
        if (edits.size() == 2) {
            edits[0]->setText("  Ноутбук  ");
            edits[1]->setText(" Семейная ");
        }
        check(dlg.deviceName() == "Ноутбук" && dlg.databaseName() == "Семейная",
              "введённые значения возвращаются без лишних пробелов");
    }

    ha::Store store(root);
    store.load();
    store.ensureIdentity();
    store.setDeviceName("Это устройство");
    int other = store.addDevice("PUBKEY-OTHER");

    auto* dlg = new DevicesDialog(store);
    dlg->show();
    auto tables = dlg->findChildren<QTableWidget*>();
    check(tables.size() == 1, "окно устройств построено");
    if (tables.isEmpty()) { delete dlg; return; }
    auto* t = tables[0];
    check(t->rowCount() == 2, "показаны оба устройства");

    // строка своего устройства: имя правится, флага «Отключено» нет
    int selfRow = -1, otherRow = -1;
    for (int r = 0; r < t->rowCount(); ++r)
        (t->item(r, 0)->data(Qt::UserRole).toInt() == store.deviceNo()
             ? selfRow : otherRow) = r;
    check(selfRow >= 0 && otherRow >= 0, "своё и чужое устройства различимы");
    check(t->item(selfRow, 1)->text() == "Это устройство",
          "имя своего устройства показано");
    check((t->item(selfRow, 1)->flags() & Qt::ItemIsEditable) != 0,
          "имя своего устройства редактируется");
    check((t->item(otherRow, 1)->flags() & Qt::ItemIsEditable) == 0,
          "имя чужого устройства не редактируется");
    check((t->item(selfRow, 2)->flags() & Qt::ItemIsUserCheckable) == 0,
          "своё устройство отключить нельзя");
    check((t->item(otherRow, 2)->flags() & Qt::ItemIsUserCheckable) != 0,
          "чужое устройство можно отключить");

    // правка имени уходит в модель и в файл
    t->item(selfRow, 1)->setText("Переименовано");
    check(store.deviceName() == "Переименовано", "новое имя записано в модель");
    t->item(otherRow, 2)->setCheckState(Qt::Checked);
    check(store.deviceDisabled("PUBKEY-OTHER"), "флаг «Отключено» установлен");
    t->item(otherRow, 2)->setCheckState(Qt::Unchecked);
    check(!store.deviceDisabled("PUBKEY-OTHER"), "флаг «Отключено» снят");
    (void)other;

    ha::Store re(root);
    re.load();
    check(re.deviceName() == "Переименовано", "имя перечитано с диска");
    dlg->close();
    delete dlg;
}

int main(int argc, char** argv) {
    alarm(90);                        // сторож: если зависнет — процесс убьют
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    fs::path base = fs::current_path() / "guitest-data";
    fs::remove_all(base);

    testMainWindow(base / "main" / ".data" / "home-accounting");
    testCatalogAndPeople(base / "cat" / ".data" / "home-accounting");
    testSyncCancel(base / "sync" / ".data" / "home-accounting");
    testFirstRunAndDevices(base / "dev" / ".data" / "home-accounting");

    std::printf("\n==== итог: %d/%d пройдено ====\n", g_total - g_fail, g_total);
    if (!g_fail) fs::remove_all(base);
    return g_fail == 0 ? 0 : 1;
}
