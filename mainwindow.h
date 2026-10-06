#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QHash>
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

    enum class RecorderRequestType {
        TargetList,
        PageData,
        DomSnapshot
    };

    struct RecorderSession {
        QString targetId;
        QString url;
        QString title;
        bool captureScheduled = false;
        bool captureInFlight = false;
        QString lastCapturedUrl;
    };

    struct RecorderRequest {
        RecorderRequestType type;
        QString captureId;
        QString captureDirectory;
        QString sessionId;
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
    void togglePageRecording();
    void startPageRecording();
    void stopPageRecording(const QString &message = QString());
    void onRecorderSocketConnected();
    void onRecorderTextMessageReceived(const QString &message);
    void onRecorderSocketDisconnected();
    int sendRecorderCommand(const QString &method,
                            const QJsonObject &parameters = {},
                            const QString &sessionId = QString());
    void handleRecorderEvent(const QJsonObject &event);
    void attachRecorderToPage(const QJsonObject &parameters);
    void schedulePageSnapshot(const QString &sessionId, int delayMilliseconds = 500);
    void capturePageSnapshot(const QString &sessionId);
    void savePageData(const RecorderRequest &request, const QJsonObject &result);
    void saveDomSnapshot(const RecorderRequest &request, const QJsonObject &result);
    void updateTrainInfoTable(const QJsonArray &trains);
    void clearTrainInfoTable();
    bool writeJsonFile(const QString &filePath, const QJsonObject &document) const;
    void appendSnapshotManifest(const QString &directory, const QJsonObject &entry) const;
    static QJsonObject redactDomSnapshot(QJsonObject snapshot);
    static QString defaultSnapshotDirectory();
    static bool isRecordablePageUrl(const QString &url);
    void setBusy(bool busy);
    void showStatus(const QString &message, bool isError = false);
    QUrl debuggerVersionUrl(QString *errorMessage = nullptr) const;
    static bool isLocalCdpHost(const QString &host);

    Ui::MainWindow *ui;
    QNetworkAccessManager m_networkManager;
    QWebSocket m_cdpSocket;
    QWebSocket m_recorderSocket;
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
    bool m_pageRecordingRequested = false;
    bool m_pageRecordingActive = false;
    int m_nextRecorderCommandId = 1;
    int m_nextSnapshotSequence = 1;
    QHash<QString, RecorderSession> m_recorderSessions;
    QHash<QString, QString> m_targetToRecorderSession;
    QHash<int, RecorderRequest> m_recorderRequests;
    QString m_trainInfoSessionId;
};
#endif // MAINWINDOW_H
