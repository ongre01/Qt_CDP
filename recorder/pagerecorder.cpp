#include "pagerecorder.h"

#include "../cdp/cdpclient.h"
#include "snapshotstorage.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QTimer>

PageRecorder::PageRecorder(CdpClient *cdpClient, SnapshotStorage *snapshotStorage, QObject *parent)
    : QObject(parent)
    , m_cdpClient(cdpClient)
    , m_snapshotStorage(snapshotStorage)
{
    connect(m_cdpClient, &CdpClient::connected, this, &PageRecorder::onConnected);
    connect(m_cdpClient, &CdpClient::disconnected, this, &PageRecorder::onDisconnected);
    connect(m_cdpClient, &CdpClient::commandResult, this, &PageRecorder::onCommandResult);
    connect(m_cdpClient, &CdpClient::commandError, this, &PageRecorder::onCommandError);
    connect(m_cdpClient, &CdpClient::eventReceived, this, &PageRecorder::onEventReceived);
    connect(m_cdpClient, &CdpClient::errorOccurred, this, [this](const QString &message) {
        if (m_monitoringRequested || m_monitoringActive) {
            m_pageRecordingActive = false;
            m_pageRecordingRequested = false;
            m_trainInfoMonitoringActive = false;
            m_trainInfoMonitoringRequested = false;
            m_monitoringActive = false;
            m_monitoringRequested = false;
            clearSessions();
            emit pageRecordingChanged(false);
            emit monitoringStopped();
            emit errorOccurred(message);
        }
    });
}

void PageRecorder::startMonitoring(const QUrl &versionUrl)
{
    if (m_trainInfoMonitoringRequested || m_trainInfoMonitoringActive) {
        return;
    }
    m_trainInfoMonitoringRequested = true;
    if (m_monitoringActive) {
        m_trainInfoMonitoringActive = true;
        emit monitoringStarted();
        return;
    }
    if (m_monitoringRequested) {
        return;
    }
    m_monitoringRequested = true;
    m_cdpClient->connectToChrome(versionUrl);
}

bool PageRecorder::startPageRecording(const QUrl &versionUrl, const QString &directory)
{
    if (directory.isEmpty()) {
        emit errorOccurred(tr("스냅샷 저장 폴더를 입력하세요."));
        return false;
    }
    if (!QDir().mkpath(directory)) {
        emit errorOccurred(tr("스냅샷 저장 폴더를 만들 수 없습니다."));
        return false;
    }
    m_snapshotStorage->setDirectory(directory);
    if (m_pageRecordingRequested || m_pageRecordingActive) {
        return true;
    }
    m_pageRecordingRequested = true;
    if (m_monitoringActive) {
        m_pageRecordingActive = true;
        emit pageRecordingChanged(true);
        emit statusChanged(tr("방문 페이지 스냅샷 저장을 시작했습니다."));
        return true;
    }
    if (!m_monitoringRequested && !m_monitoringActive) {
        m_monitoringRequested = true;
        m_cdpClient->connectToChrome(versionUrl);
    }
    emit statusChanged(tr("방문 페이지 기록용 CDP 연결을 준비하는 중입니다..."));
    return true;
}

void PageRecorder::stopPageRecording()
{
    const bool wasRecording = m_pageRecordingRequested || m_pageRecordingActive;
    m_pageRecordingRequested = false;
    m_pageRecordingActive = false;
    if (wasRecording) {
        emit pageRecordingChanged(false);
    }
    if (m_trainInfoMonitoringRequested || m_trainInfoMonitoringActive) {
        return;
    }
    stop();
}

void PageRecorder::stop()
{
    const bool wasActive = isMonitoringRequested() || isMonitoringActive();
    m_pageRecordingRequested = false;
    m_pageRecordingActive = false;
    m_trainInfoMonitoringRequested = false;
    m_trainInfoMonitoringActive = false;
    m_monitoringRequested = false;
    m_monitoringActive = false;
    clearSessions();
    m_cdpClient->close();
    emit pageRecordingChanged(false);
    if (wasActive) {
        emit monitoringStopped();
    }
}

bool PageRecorder::isMonitoringRequested() const
{
    return m_pageRecordingRequested || m_trainInfoMonitoringRequested;
}

bool PageRecorder::isMonitoringActive() const
{
    return m_pageRecordingActive || m_trainInfoMonitoringActive;
}

bool PageRecorder::isPageRecordingActive() const
{
    return m_pageRecordingActive;
}

bool PageRecorder::hasSession(const QString &sessionId) const
{
    return m_sessions.contains(sessionId);
}

bool PageRecorder::reloadPage(const QString &sessionId)
{
    return !sessionId.isEmpty() && hasSession(sessionId)
           && sendCommand(QStringLiteral("Page.reload"), {}, sessionId) != 0;
}

void PageRecorder::onConnected()
{
    if (!m_monitoringRequested) {
        m_cdpClient->close();
        return;
    }
    m_monitoringActive = true;
    m_trainInfoMonitoringActive = m_trainInfoMonitoringRequested;
    if (m_pageRecordingRequested) {
        m_pageRecordingActive = true;
        emit pageRecordingChanged(true);
    }
    sendCommand(QStringLiteral("Target.setDiscoverTargets"), {{QStringLiteral("discover"), true}});
    sendCommand(QStringLiteral("Target.setAutoAttach"),
                {{QStringLiteral("autoAttach"), true},
                 {QStringLiteral("waitForDebuggerOnStart"), false},
                 {QStringLiteral("flatten"), true},
                 {QStringLiteral("filter"), QJsonArray {QJsonObject {{QStringLiteral("type"), QStringLiteral("page")}}}}});
    const int commandId = sendCommand(QStringLiteral("Target.getTargets"));
    if (commandId != 0) {
        m_requests.insert(commandId, {RequestType::TargetList, {}, {}, false});
    }
    if (m_trainInfoMonitoringActive) {
        emit monitoringStarted();
    }
    emit statusChanged(m_pageRecordingActive
                           ? tr("방문 페이지 기록 중입니다. 입력값과 textarea 값은 저장하지 않습니다.")
                           : tr("열차 정보 수집 중입니다. 열차 조회 페이지를 열면 목록을 표시합니다."));
}

void PageRecorder::onDisconnected()
{
    const bool wasActive = m_monitoringRequested || m_monitoringActive;
    m_pageRecordingActive = false;
    m_pageRecordingRequested = false;
    m_trainInfoMonitoringActive = false;
    m_trainInfoMonitoringRequested = false;
    m_monitoringActive = false;
    m_monitoringRequested = false;
    clearSessions();
    emit pageRecordingChanged(false);
    if (wasActive) {
        emit monitoringStopped();
        emit errorOccurred(tr("열차 정보 수집용 CDP 연결이 끊어졌습니다."));
    }
}

void PageRecorder::onCommandResult(int id, const QJsonObject &result)
{
    if (!m_requests.contains(id)) {
        return;
    }
    const Request request = m_requests.take(id);
    if (request.type == RequestType::TargetList) {
        const QJsonArray targetInfos = result.value(QStringLiteral("targetInfos")).toArray();
        for (const QJsonValue &value : targetInfos) {
            const QJsonObject targetInfo = value.toObject();
            if (targetInfo.value(QStringLiteral("type")).toString() != QStringLiteral("page")) {
                continue;
            }
            const QString targetId = targetInfo.value(QStringLiteral("targetId")).toString();
            if (!targetId.isEmpty() && !m_targetToSession.contains(targetId)) {
                sendCommand(QStringLiteral("Target.attachToTarget"),
                            {{QStringLiteral("targetId"), targetId}, {QStringLiteral("flatten"), true}});
            }
        }
        return;
    }

    if (m_sessions.contains(request.sessionId)) {
        m_sessions[request.sessionId].captureInFlight = false;
    }
    if (request.saveSnapshot) {
        const Session session = m_sessions.value(request.sessionId);
        if (!m_snapshotStorage->save(request.captureId, session.url, session.title, result)) {
            emit errorOccurred(tr("DOM 스냅샷을 저장하지 못했습니다."));
        } else {
            emit statusChanged(tr("DOM 스냅샷을 저장했습니다: %1").arg(session.title));
        }
    }
    emit snapshotCaptured(result, request.sessionId);
}

void PageRecorder::onCommandError(int id, const QString &message)
{
    if (!m_requests.contains(id)) {
        return;
    }
    const Request request = m_requests.take(id);
    if (request.type == RequestType::DomSnapshot && m_sessions.contains(request.sessionId)) {
        m_sessions[request.sessionId].captureInFlight = false;
    }
    emit errorOccurred(tr("CDP 명령을 실행하지 못했습니다: %1").arg(message));
}

void PageRecorder::onEventReceived(const QString &method, const QJsonObject &parameters,
                                   const QString &eventSessionId)
{
    if (!isMonitoringActive()) {
        return;
    }
    if (method == QStringLiteral("Page.javascriptDialogOpening")) {
        const QString dialogType = parameters.value(QStringLiteral("type")).toString();
        if (dialogType == QStringLiteral("alert") && !eventSessionId.isEmpty()) {
            sendCommand(QStringLiteral("Page.handleJavaScriptDialog"), {{QStringLiteral("accept"), true}}, eventSessionId);
            emit statusChanged(tr("안내 메시지를 자동으로 확인했습니다."));
        }
        return;
    }
    if (method == QStringLiteral("Target.attachedToTarget")) {
        attachToPage(parameters);
        return;
    }
    if (method == QStringLiteral("Target.detachedFromTarget")) {
        const QString sessionId = parameters.value(QStringLiteral("sessionId")).toString();
        if (m_sessions.contains(sessionId)) {
            m_targetToSession.remove(m_sessions.value(sessionId).targetId);
            m_sessions.remove(sessionId);
        }
        emit pageDetached(sessionId);
        return;
    }
    if (method == QStringLiteral("Target.targetInfoChanged")) {
        const QJsonObject targetInfo = parameters.value(QStringLiteral("targetInfo")).toObject();
        const QString sessionId = m_targetToSession.value(targetInfo.value(QStringLiteral("targetId")).toString());
        if (!sessionId.isEmpty() && m_sessions.contains(sessionId)) {
            Session &session = m_sessions[sessionId];
            session.url = targetInfo.value(QStringLiteral("url")).toString();
            session.title = targetInfo.value(QStringLiteral("title")).toString();
        }
        return;
    }
    if (method == QStringLiteral("Network.loadingFinished")) {
        schedulePageSnapshot(eventSessionId, 700);
    } else if (method == QStringLiteral("Page.loadEventFired")
               || method == QStringLiteral("Page.navigatedWithinDocument")) {
        schedulePageSnapshot(eventSessionId);
    }
}

void PageRecorder::attachToPage(const QJsonObject &parameters)
{
    const QJsonObject targetInfo = parameters.value(QStringLiteral("targetInfo")).toObject();
    if (targetInfo.value(QStringLiteral("type")).toString() != QStringLiteral("page")) {
        return;
    }
    const QString sessionId = parameters.value(QStringLiteral("sessionId")).toString();
    const QString targetId = targetInfo.value(QStringLiteral("targetId")).toString();
    if (sessionId.isEmpty() || targetId.isEmpty()) {
        return;
    }
    if (m_targetToSession.contains(targetId)) {
        sendCommand(QStringLiteral("Target.detachFromTarget"), {{QStringLiteral("sessionId"), sessionId}});
        return;
    }
    m_sessions.insert(sessionId, {targetId, targetInfo.value(QStringLiteral("url")).toString(),
                                  targetInfo.value(QStringLiteral("title")).toString(), false, false});
    m_targetToSession.insert(targetId, sessionId);
    sendCommand(QStringLiteral("Page.enable"), {}, sessionId);
    sendCommand(QStringLiteral("Network.enable"), {}, sessionId);
    schedulePageSnapshot(sessionId, 800);
}

void PageRecorder::schedulePageSnapshot(const QString &sessionId, int delayMilliseconds)
{
    if (!isMonitoringActive() || !m_sessions.contains(sessionId)) {
        return;
    }
    Session &session = m_sessions[sessionId];
    if (session.captureScheduled || session.captureInFlight
        || !SnapshotStorage::isRecordablePageUrl(session.url)) {
        return;
    }
    session.captureScheduled = true;
    QTimer::singleShot(delayMilliseconds, this, [this, sessionId]() {
        if (!isMonitoringActive() || !m_sessions.contains(sessionId)) {
            return;
        }
        m_sessions[sessionId].captureScheduled = false;
        capturePageSnapshot(sessionId);
    });
}

void PageRecorder::capturePageSnapshot(const QString &sessionId)
{
    if (!isMonitoringActive() || !m_sessions.contains(sessionId)) {
        return;
    }
    Session &session = m_sessions[sessionId];
    if (session.captureInFlight) {
        return;
    }
    const bool saveSnapshot = m_pageRecordingActive;
    const QString captureId = saveSnapshot
        ? QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"))
              + QStringLiteral("-%1").arg(m_nextSnapshotSequence++)
        : QString();
    if (saveSnapshot && !m_snapshotStorage->prepare(captureId)) {
        emit errorOccurred(tr("페이지 스냅샷 폴더를 만들 수 없습니다."));
        return;
    }
    session.captureInFlight = true;
    const int commandId = sendCommand(QStringLiteral("DOMSnapshot.captureSnapshot"),
                                      {{QStringLiteral("computedStyles"), QJsonArray {}},
                                       {QStringLiteral("includeDOMRects"), true}}, sessionId);
    if (commandId == 0) {
        session.captureInFlight = false;
        return;
    }
    m_requests.insert(commandId, {RequestType::DomSnapshot, captureId, sessionId, saveSnapshot});
}

void PageRecorder::clearSessions()
{
    m_requests.clear();
    m_sessions.clear();
    m_targetToSession.clear();
}

int PageRecorder::sendCommand(const QString &method, const QJsonObject &parameters,
                              const QString &sessionId)
{
    return m_cdpClient->sendCommand(method, parameters, sessionId);
}
