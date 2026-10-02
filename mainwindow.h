#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    enum class KorailLoginStep {
        Idle,
        CreatingTarget,
        AttachingTarget,
        EnablingPage,
        Navigating,
        WaitingForLoginForm,
        SubmittingLogin,
        CheckingLogin
    };

    void startChromeForCdp();
    void checkStartedChromeEndpoint();
    void startKorailAutoLogin();
    void onCdpSocketConnected();
    void onCdpTextMessageReceived(const QString &message);
    void onCdpSocketDisconnected();
    void sendCdpCommand(const QString &method, const QJsonObject &parameters = {});
    void waitForKorailLoginForm();
    void submitKorailLogin();
    void checkKorailLoginResult();
    void finishKorailLogin(const QString &message, bool isError = false);
    void setBusy(bool busy);
    void showStatus(const QString &message, bool isError = false);
    QUrl debuggerVersionUrl(QString *errorMessage = nullptr) const;
    static bool isLocalCdpHost(const QString &host);

    Ui::MainWindow *ui;
    QNetworkAccessManager m_networkManager;
    QWebSocket m_cdpSocket;
    QTimer m_cdpReadyTimer;
    int m_cdpReadyAttempts = 0;
    bool m_startupRequestInFlight = false;
    KorailLoginStep m_korailLoginStep = KorailLoginStep::Idle;
    int m_nextCdpCommandId = 1;
    int m_pendingCdpCommandId = 0;
    int m_korailFormCheckAttempts = 0;
    int m_korailResultCheckAttempts = 0;
    bool m_korailLoginInProgress = false;
    QString m_cdpSessionId;
};
#endif // MAINWINDOW_H
