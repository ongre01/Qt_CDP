#ifndef PAGERECORDER_H
#define PAGERECORDER_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QUrl>

class CdpClient;
class SnapshotStorage;

class PageRecorder : public QObject
{
    Q_OBJECT

public:
    explicit PageRecorder(CdpClient *cdpClient, SnapshotStorage *snapshotStorage,
                          QObject *parent = nullptr);

    void startMonitoring(const QUrl &versionUrl);
    bool startPageRecording(const QUrl &versionUrl, const QString &directory);
    void stopPageRecording();
    void stop();

    bool isMonitoringRequested() const;
    bool isMonitoringActive() const;
    bool isPageRecordingActive() const;
    bool hasSession(const QString &sessionId) const;
    bool reloadPage(const QString &sessionId);

signals:
    void monitoringStarted();
    void monitoringStopped();
    void pageRecordingChanged(bool active);
    void snapshotCaptured(const QJsonObject &snapshot, const QString &sessionId);
    void pageDetached(const QString &sessionId);
    void statusChanged(const QString &message);
    void errorOccurred(const QString &message);

private:
    enum class RequestType { TargetList, DomSnapshot };
    struct Session {
        QString targetId;
        QString url;
        QString title;
        bool captureScheduled = false;
        bool captureInFlight = false;
    };
    struct Request {
        RequestType type;
        QString captureId;
        QString sessionId;
        bool saveSnapshot = false;
    };

    void onConnected();
    void onDisconnected();
    void onCommandResult(int id, const QJsonObject &result);
    void onCommandError(int id, const QString &message);
    void onEventReceived(const QString &method, const QJsonObject &parameters,
                         const QString &sessionId);
    void attachToPage(const QJsonObject &parameters);
    void schedulePageSnapshot(const QString &sessionId, int delayMilliseconds = 500);
    void capturePageSnapshot(const QString &sessionId);
    void clearSessions();
    int sendCommand(const QString &method, const QJsonObject &parameters = {},
                    const QString &sessionId = {});

    CdpClient *m_cdpClient;
    SnapshotStorage *m_snapshotStorage;
    bool m_pageRecordingRequested = false;
    bool m_pageRecordingActive = false;
    bool m_trainInfoMonitoringRequested = false;
    bool m_trainInfoMonitoringActive = false;
    bool m_monitoringRequested = false;
    bool m_monitoringActive = false;
    QHash<QString, Session> m_sessions;
    QHash<QString, QString> m_targetToSession;
    QHash<int, Request> m_requests;
    int m_nextSnapshotSequence = 1;
};

#endif // PAGERECORDER_H
