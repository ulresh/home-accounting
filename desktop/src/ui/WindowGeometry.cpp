#include "WindowGeometry.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSettings>
#include <QWidget>

namespace {

constexpr auto kGroup = "windows";

// Хранитель геометрии живёт как ребёнок своего окна: восстанавливает состояние
// в конструкторе и сохраняет его при каждом скрытии окна. Скрытие ловим, а не
// closeEvent: QDialog::accept()/reject() закрывают окно БЕЗ closeEvent, и по
// нему геометрия диалогов не сохранялась бы.
class Keeper : public QObject {
public:
    Keeper(QWidget* w, QString key) : QObject(w), w_(w), key_(std::move(key)) {
        w_->installEventFilter(this);
        QSettings s;
        s.beginGroup(kGroup);
        // Пусто или несовместимо (другая версия Qt) — restoreGeometry вернёт
        // false и оставит размеры, заданные конструктором окна.
        w_->restoreGeometry(s.value(key_).toByteArray());
        // Выход из приложения при открытом окне: Hide не придёт, а окно ещё
        // живо. Контекст — сам хранитель, поэтому связь снимается вместе с окном.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { save(); });
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == w_ && e->type() == QEvent::Hide) save();
        return QObject::eventFilter(o, e);
    }

private:
    // saveGeometry() сам хранит «нормальную» геометрию вместе с признаками
    // распахнутости/полноэкранности и экраном, на котором окно стояло.
    void save() {
        QSettings s;
        s.beginGroup(kGroup);
        s.setValue(key_, w_->saveGeometry());
    }

    QWidget* w_;
    QString key_;
};

}

void ha::ui::rememberGeometry(QWidget* w, const QString& key) {
    if (w && !key.isEmpty()) new Keeper(w, key);   // владеет окно
}
