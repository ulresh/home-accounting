#include "SyncDialog.h"
#include "../model/Store.h"
#include "../sync/QrCode.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <QCloseEvent>

static QPixmap renderQr(const QString& text, int targetPx = 280) {
    try {
        qr::QrCode qc = qr::QrCode::encode(text.toStdString(), qr::Ecc::MEDIUM);
        int n = qc.size();
        int border = 4;
        int dim = n + border * 2;
        QImage img(dim, dim, QImage::Format_RGB32);
        img.fill(Qt::white);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qc.module(x, y))
                    img.setPixel(x + border, y + border, qRgb(0, 0, 0));
        return QPixmap::fromImage(img.scaled(targetPx, targetPx, Qt::KeepAspectRatio, Qt::FastTransformation));
    } catch (...) {
        return QPixmap();
    }
}

SyncDialog::SyncDialog(ha::Store& store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Синхронизация"));
    setMinimumWidth(420);

    auto* root = new QVBoxLayout(this);

    // --- роль сервера: показать QR ---
    auto* g1 = new QGroupBox(tr("Принять подключение (показать QR)"), this);
    auto* l1 = new QVBoxLayout(g1);
    serverBtn_ = new QPushButton(tr("Старт"), g1);
    qrLabel_ = new QLabel(g1);
    qrLabel_->setAlignment(Qt::AlignCenter);
    infoLabel_ = new QLabel(g1);
    infoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLabel_->setWordWrap(true);
    l1->addWidget(serverBtn_);
    l1->addWidget(qrLabel_);
    l1->addWidget(infoLabel_);
    root->addWidget(g1);

    // --- роль клиента: подключиться ---
    auto* g2 = new QGroupBox(tr("Подключиться к устройству"), this);
    auto* f2 = new QFormLayout(g2);
    ip_ = new QLineEdit(g2);
    port_ = new QLineEdit(g2);
    code_ = new QLineEdit(g2);
    paste_ = new QLineEdit(g2);
    paste_->setPlaceholderText(tr("или вставьте JSON реквизитов"));
    clientBtn_ = new QPushButton(tr("Синхронизировать"), g2);
    f2->addRow(tr("IP:"), ip_);
    f2->addRow(tr("Порт:"), port_);
    f2->addRow(tr("Код:"), code_);
    f2->addRow(tr("JSON:"), paste_);
    f2->addRow(clientBtn_);
    root->addWidget(g2);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(serverBtn_, &QPushButton::clicked, this, &SyncDialog::startServer);
    connect(clientBtn_, &QPushButton::clicked, this, &SyncDialog::startClient);
    connect(this, &SyncDialog::serverReady, this, &SyncDialog::onServerReady, Qt::QueuedConnection);
    connect(this, &SyncDialog::finished, this, &SyncDialog::onFinished, Qt::QueuedConnection);
}

SyncDialog::~SyncDialog() {
    closing_ = true;
    if (server_) server_->cancel();
    if (client_) client_->cancel();
    if (worker_.joinable()) worker_.join();
}

void SyncDialog::closeEvent(QCloseEvent* e) {
    // Закрытие окна (в т.ч. командой оконной системы) прерывает синхронизацию
    // в любой момент — и приём (server), и подключение (client).
    closing_ = true;
    if (server_) server_->cancel();
    if (client_) client_->cancel();
    QDialog::closeEvent(e);
}

bool SyncDialog::askConfirm(const QString& pubkey) {
    if (closing_) return false;          // не блокироваться при закрытии
    bool result = false;
    QMetaObject::invokeMethod(this, [&]() {
        auto r = QMessageBox::question(this, tr("Новое устройство"),
            tr("Разрешить синхронизацию с новым устройством?\n\n") + pubkey.left(48) + "…",
            QMessageBox::Yes | QMessageBox::No);
        result = (r == QMessageBox::Yes);
    }, Qt::BlockingQueuedConnection);
    return result;
}

void SyncDialog::startServer() {
    if (busy_) return;
    busy_ = true;
    serverBtn_->setEnabled(false);
    clientBtn_->setEnabled(false);
    status_->setText(tr("Ожидание подключения…"));

    server_ = std::make_unique<ha::SyncServer>(store_);
    ha::PairInfo info;
    try {
        info = server_->listen();
    } catch (const std::exception& e) {
        status_->setText(tr("Ошибка: ") + e.what());
        busy_ = false; serverBtn_->setEnabled(true); clientBtn_->setEnabled(true);
        return;
    }
    emit serverReady(QString::fromStdString(info.toJson()));

    worker_ = std::thread([this]() {
        auto confirm = [this](const std::string& pk) { return askConfirm(QString::fromStdString(pk)); };
        ha::SyncResult r = server_->wait(confirm);
        QString msg = r.ok
            ? tr("Передано %1, принято %2").arg(r.sent).arg(r.received)
            : tr("Не выполнено");
        emit finished(r.ok, msg, QString::fromStdString(r.error), QString::fromStdString(r.peerDb));
    });
}

void SyncDialog::onServerReady(QString infoJson) {
    qrLabel_->setPixmap(renderQr(infoJson));
    auto info = ha::PairInfo::fromJson(infoJson.toStdString());
    infoLabel_->setText(tr("IP: %1   Порт: %2\nКод: %3\nБаза: %4")
        .arg(QString::fromStdString(info.ip))
        .arg(info.port)
        .arg(QString::fromStdString(info.code))
        .arg(QString::fromStdString(info.db)));
}

void SyncDialog::startClient() {
    if (busy_) return;
    ha::PairInfo info;
    QString js = paste_->text().trimmed();
    if (!js.isEmpty()) {
        info = ha::PairInfo::fromJson(js.toStdString());
    } else {
        info.ip = ip_->text().trimmed().toStdString();
        info.port = port_->text().trimmed().toInt();
        info.code = code_->text().trimmed().toStdString();
        info.db = store_.database();
    }
    if (info.ip.empty() || info.port == 0 || info.code.empty()) {
        QMessageBox::warning(this, tr("Синхронизация"), tr("Укажите IP, порт и код."));
        return;
    }
    busy_ = true;
    serverBtn_->setEnabled(false);
    clientBtn_->setEnabled(false);
    status_->setText(tr("Подключение…"));

    client_ = std::make_unique<ha::SyncClient>(store_);
    worker_ = std::thread([this, info]() {
        auto confirm = [this](const std::string& pk) { return askConfirm(QString::fromStdString(pk)); };
        ha::SyncResult r = client_->connect(info, confirm);
        QString msg = r.ok
            ? tr("Передано %1, принято %2").arg(r.sent).arg(r.received)
            : tr("Не выполнено");
        emit finished(r.ok, msg, QString::fromStdString(r.error), QString::fromStdString(r.peerDb));
    });
}

void SyncDialog::onFinished(bool ok, QString message, QString error, QString peerDb) {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    serverBtn_->setEnabled(true);
    clientBtn_->setEnabled(true);

    if (ok) {
        status_->setText("✓ " + message);
        QMessageBox::information(this, tr("Синхронизация"), message);
    } else if (error == "db_mismatch") {
        auto r = QMessageBox::question(this, tr("Разные базы"),
            tr("У партнёра база «%1», у вас «%2».\nПереключиться на «%1»?")
                .arg(peerDb).arg(QString::fromStdString(store_.database())),
            QMessageBox::Yes | QMessageBox::No);
        if (r == QMessageBox::Yes) {
            store_.switchDatabase(peerDb.toStdString(), true);
            status_->setText(tr("База переключена на «%1». Повторите синхронизацию.").arg(peerDb));
        }
    } else {
        status_->setText(tr("Ошибка: ") + (error.isEmpty() ? message : error));
    }
}
