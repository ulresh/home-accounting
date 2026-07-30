#pragma once
#include <QDialog>
#include "../model/Types.h"

class QComboBox;
class QDoubleSpinBox;
class QDateTimeEdit;
class QLineEdit;
class QCheckBox;

namespace ha { class Store; }

class EventDialog : public QDialog {
    Q_OBJECT
public:
    EventDialog(ha::Store& store, const ha::Event* edit, QWidget* parent = nullptr);

    QString eventDateTime() const;
    QString subject() const;
    double  cost() const;
    // Пустая строка = поле не заполнено (модель хранит поля как std::string).
    QString people() const;
    QString volume() const;
    QString comment() const;

private:
    ha::Store& store_;
    QComboBox*      subject_;
    QDoubleSpinBox* cost_;
    QDateTimeEdit*  when_;
    QCheckBox*      withTime_;
    QComboBox*      people_;
    QLineEdit*      volume_;
    QLineEdit*      comment_;
};
