#include "cdpclient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QWebSocket>

CdpClient::CdpClient(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_readyTimer(new QTimer(this))
{
    m_readyTimer->setInterval(200);
    connect(m_readyTimer, &QTimer::timeout, this, &CdpClient::checkStartedChromeEndpoint);
    connect(m_socket, &QWebSocket::connected, this, &CdpClient::connected);
    connect(m_socket, &QWebSocket::disconnected, this, &CdpClient::disconnected);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &CdpClient::handleTextMessage);
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(m_socket->errorString());
    });
}

QUrl CdpClient::debuggerVersionUrl(const QString &endpoint, QString *errorMessage)
{
    QString endpointText = endpoint.trimmed();
    if (!endpointText.contains(QStringLiteral("://"))) {
        endpointText.prepend(QStringLiteral("http://"));
    }

    QUrl versionUrl(endpointText);
    if (!versionUrl.isValid() || versionUrl.host().isEmpty()
        || (versionUrl.scheme() != QStringLiteral("http")
            && versionUrl.scheme() != QStringLiteral("https"))) {
        if (errorMessage) {
            *errorMessage = tr("유효한 HTTP(S) CDP 디버거 주소를 입력하세요.");
        }
        return {};
    }
    if (versionUrl.port() == -1) {
        versionUrl.setPort(9222);
    }
    versionUrl.setPath(QStringLiteral("/json/version"));
    versionUrl.setQuery({});
    return versionUrl;
}

bool CdpClient::isLocalHost(const QString &host)
{
    const QString normalizedHost = host.toLower();
    return normalizedHost == QStringLiteral("127.0.0.1")
           || normalizedHost == QStringLiteral("localhost")
           || normalizedHost == QStringLiteral("::1");
}

void CdpClient::startChrome(const QString &executable, const QString &userDataDirectory,
                            const QUrl &versionUrl)
{
    if (executable.isEmpty() || !QFileInfo::exists(executable)) {
        emit errorOccurred(tr("Chrome 실행 파일을 찾을 수 없습니다."));
        return;
    }
    if (userDataDirectory.isEmpty()) {
        emit errorOccurred(tr("CDP 전용 사용자 데이터 디렉터리를 입력하세요."));
        return;
    }
    if (!versionUrl.isValid() || !isLocalHost(versionUrl.host())) {
        emit errorOccurred(tr("Chrome 시작은 로컬 CDP 주소만 지원합니다."));
        return;
    }

    const QStringList arguments {
        QStringLiteral("--remote-debugging-port=%1").arg(versionUrl.port()),
        QStringLiteral("--user-data-dir=%1").arg(QDir::cleanPath(userDataDirectory))
    };
    qint64 processId = 0;
    if (!QProcess::startDetached(executable, arguments, QString(), &processId)) {
        emit errorOccurred(tr("Chrome을 시작하지 못했습니다."));
        return;
    }

    m_startupVersionUrl = versionUrl;
    m_readyAttempts = 0;
    m_startupRequestInFlight = false;
    m_readyTimer->start();
    emit chromeStarted(processId);
}

void CdpClient::connectToChrome(const QUrl &versionUrl)
{
    if (!versionUrl.isValid()) {
        emit errorOccurred(tr("유효한 CDP 디버거 주소가 아닙니다."));
        return;
    }
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
    requestWebSocketUrl(versionUrl, true);
}

void CdpClient::close()
{
    m_readyTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
}

int CdpClient::sendCommand(const QString &method, const QJsonObject &params,
                           const QString &sessionId)
{
    if (!isConnected()) {
        return 0;
    }

    const int commandId = m_nextCommandId++;
    QJsonObject command {{QStringLiteral("id"), commandId},
                         {QStringLiteral("method"), method}};
    if (!params.isEmpty()) {
        command.insert(QStringLiteral("params"), params);
    }
    if (!sessionId.isEmpty()) {
        command.insert(QStringLiteral("sessionId"), sessionId);
    }
    m_socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(command).toJson(QJsonDocument::Compact)));
    return commandId;
}

bool CdpClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void CdpClient::checkStartedChromeEndpoint()
{
    if (m_startupRequestInFlight) {
        return;
    }
    constexpr int maxAttempts = 50;
    if (++m_readyAttempts > maxAttempts) {
        m_readyTimer->stop();
        emit errorOccurred(tr("Chrome CDP 엔드포인트가 10초 안에 준비되지 않았습니다."));
        return;
    }
    m_startupRequestInFlight = true;
    requestWebSocketUrl(m_startupVersionUrl, false);
}

void CdpClient::requestWebSocketUrl(const QUrl &versionUrl, bool openSocketWhenReady)
{
    QNetworkReply *reply = m_networkManager->get(QNetworkRequest(versionUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply, openSocketWhenReady]() {
        const QByteArray body = reply->readAll();
        const bool requestSucceeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_startupRequestInFlight = false;

        const QUrl webSocketUrl(QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("webSocketDebuggerUrl"))
                                    .toString());
        const bool validSocketUrl = webSocketUrl.isValid()
                                    && (webSocketUrl.scheme() == QStringLiteral("ws")
                                        || webSocketUrl.scheme() == QStringLiteral("wss"));
        if (!requestSucceeded || !validSocketUrl) {
            if (openSocketWhenReady) {
                emit errorOccurred(tr("CDP Chrome에 연결하지 못했습니다. 먼저 CDP용 Chrome을 시작하세요."));
            }
            return;
        }
        if (openSocketWhenReady) {
            m_socket->open(webSocketUrl);
        } else {
            m_readyTimer->stop();
            emit chromeEndpointReady(webSocketUrl);
        }
    });
}

void CdpClient::handleTextMessage(const QString &message)
{
    const QJsonObject response = QJsonDocument::fromJson(message.toUtf8()).object();
    if (response.isEmpty()) {
        return;
    }
    if (!response.contains(QStringLiteral("id"))) {
        emit eventReceived(response.value(QStringLiteral("method")).toString(),
                           response.value(QStringLiteral("params")).toObject(),
                           response.value(QStringLiteral("sessionId")).toString());
        return;
    }

    const int id = response.value(QStringLiteral("id")).toInt();
    const QJsonObject error = response.value(QStringLiteral("error")).toObject();
    if (!error.isEmpty()) {
        emit commandError(id, error.value(QStringLiteral("message")).toString());
        return;
    }
    emit commandResult(id, response.value(QStringLiteral("result")).toObject());
}
