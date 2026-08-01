#pragma once
#include <QDialog>
#include <memory>
#include "../sync/SyncService.h"

class QCloseEvent;

class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;

namespace ha { class Store; }

// Синхронизация идёт в главном цикле событий: отдельного потока нет,
// результат приходит callback'ом из ha::SyncServer / ha::SyncClient.
class SyncDialog : public QDialog {
    Q_OBJECT
public:
    explicit SyncDialog(ha::Store& store, QWidget* parent = nullptr);
    ~SyncDialog() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void startServer();
    void startClient();

private:
    void showPairInfo(const QString& infoJson);
    void onPeer(const QString& name);             // собеседник опознан
    static QString peerTitle(const QString& name);
    void onFinished(const ha::SyncResult& res);   // callback из SyncService
    void showResult(const ha::SyncResult& r);     // реакция в цикле событий
    bool askConfirm(const QString& pubkey);

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
    std::unique_ptr<ha::SyncClient> client_;
    bool busy_ = false;
    bool closing_ = false;
};
