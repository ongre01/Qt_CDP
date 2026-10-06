#ifndef KORAILAUTHCONTROLLER_H
#define KORAILAUTHCONTROLLER_H

#include <QObject>
#include <QJsonObject>

class CdpClient;

class KorailAuthController : public QObject
{
    Q_OBJECT

public:
    explicit KorailAuthController(CdpClient *cdpClient, QObject *parent = nullptr);

    void login(const QString &id, const QString &password, const QUrl &versionUrl);
    bool isLoggingIn() const;

signals:
    void loginStarted();
    void loginSucceeded();
    void loginFailed(const QString &reason);
    void statusChanged(const QString &message);

private:
    enum class LoginStep {
        Idle,
        CreatingTarget,
        AttachingTarget,
        EnablingPage,
        Navigating,
        WaitingForLoginForm,
        SubmittingLogin,
        CheckingLogin
    };

    void onConnected();
    void onCommandResult(int id, const QJsonObject &result);
    void onCommandError(int id, const QString &message);
    void waitForLoginForm();
    void submitLogin();
    void checkLoginResult();
    void finish(const QString &message, bool failed);
    int sendCommand(const QString &method, const QJsonObject &parameters = {});

    CdpClient *m_cdpClient;
    LoginStep m_step = LoginStep::Idle;
    int m_pendingCommandId = 0;
    int m_formCheckAttempts = 0;
    int m_resultCheckAttempts = 0;
    bool m_inProgress = false;
    QString m_sessionId;
    QString m_memberNumber;
    QString m_password;
};

#endif // KORAILAUTHCONTROLLER_H
