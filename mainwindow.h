#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QTableWidgetItem;

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
        DomSnapshot,
        AutoBooking,
        AutoBookingMousePressed,
        AutoBookingMouseReleased,
        AutoBookingConfirm,
        AutoBookingConfirmMousePressed,
        AutoBookingConfirmMouseReleased,
        AutoBookingDismissDialog,
        AutoBookingDismissDialogMousePressed,
        AutoBookingDismissDialogMouseReleased
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
        bool saveSnapshot = false;
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
    void openSnapshotDirectory();
    void startPageRecording();
    void stopPageRecording(const QString &message = QString());
    void startTrainInfoMonitoring();
    bool startRecorderConnection();
    bool isRecorderRequested() const;
    bool isRecorderActive() const;
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
    void saveDomSnapshot(const RecorderRequest &request, const QJsonObject &result);
    void updateTrainInfoTable(const QJsonArray &trains);
    void clearTrainInfoTable();
    void onTrainInfoItemChanged(QTableWidgetItem *item);
    void startTrainRefreshMacro();
    void stopTrainRefreshMacro(const QString &message = QString());
    void updateSelectedTrainRefresh();
    void refreshSelectedTrainPage();
    bool hasReservableSelectedTrain() const;
    QJsonObject reservableSelectedTrain() const;
    void startAutoBookingForReservableTrain();
    void continueAutoBookingWithConfirmation(const QString &sessionId);
    void continueAutoBookingWithInformationalDialogs(const QString &sessionId);
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
    QTimer m_selectedTrainRefreshTimer;
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
    bool m_trainInfoMonitoringRequested = false;
    bool m_trainInfoMonitoringActive = false;
    int m_nextRecorderCommandId = 1;
    int m_nextSnapshotSequence = 1;
    QHash<QString, RecorderSession> m_recorderSessions;
    QHash<QString, QString> m_targetToRecorderSession;
    QHash<int, RecorderRequest> m_recorderRequests;
    QString m_trainInfoSessionId;
    QSet<QString> m_selectedTrainKeys;
    bool m_trainRefreshMacroActive = false;
    bool m_autoBookingInProgress = false;
    QString m_autoBookingSeatType;
    double m_autoBookingClickX = 0.0;
    double m_autoBookingClickY = 0.0;
    int m_autoBookingConfirmationAttempts = 0;
    int m_autoBookingDialogAttempts = 0;
    int m_autoBookingDialogClicks = 0;
    bool m_autoBookingDialogDomFallbackUsed = false;
    bool m_autoBookingWaitingForSelectionNotice = false;
    bool m_updatingTrainInfoTable = false;
};
#endif // MAINWINDOW_H
