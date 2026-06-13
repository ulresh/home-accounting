#pragma once
#include <QDialog>
#include <thread>
#include <atomic>
#include <memory>
#include "../sync/SyncService.h"

class QCloseEvent;

class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;

namespace ha { class Store; }

class SyncDialog : public QDialog {
    Q_OBJECT
public:
    explicit SyncDialog(ha::Store& store, QWidget* parent = nullptr);
    ~SyncDialog() override;

protected:
    void closeEvent(QCloseEvent* e) override;

signals:
    void serverReady(QString infoJson);
    void finished(bool ok, QString message, QString error, QString peerDb);

private slots:
    void startServer();
    void startClient();
    void onServerReady(QString infoJson);
    void onFinished(bool ok, QString message, QString error, QString peerDb);

private:
    bool askConfirm(const QString& pubkey);   // блокирующий вызов из рабочего потока

    ha::Store& store_;
    QLabel*      qrLabel_;
    QLabel*      infoLabel_;
    QLineEdit*   ip_;
    QLineEdit*   port_;
    QLineEdit*   code_;
    QLineEdit*   paste_;
    QPushButton* serverBtn_;
    QPushButton* clientBtn_;
    QLabel*      status_;

    std::unique_ptr<ha::SyncServer> server_;
    std::thread worker_;
    bool busy_ = false;
    std::atomic<bool> closing_{false};
};
