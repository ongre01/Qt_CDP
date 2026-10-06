#ifndef CDPCLIENT_H
#define CDPCLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QUrl>

class QNetworkAccessManager;
class QTimer;
class QWebSocket;

class CdpClient : public QObject
{
    Q_OBJECT

public:
    explicit CdpClient(QObject *parent = nullptr);

    static QUrl debuggerVersionUrl(const QString &endpoint, QString *errorMessage = nullptr);
    static bool isLocalHost(const QString &host);

    void startChrome(const QString &executable, const QString &userDataDirectory,
                     const QUrl &versionUrl);
    void connectToChrome(const QUrl &versionUrl);
    void close();

    int sendCommand(const QString &method, const QJsonObject &params = {},
                    const QString &sessionId = {});
    bool isConnected() const;

signals:
    void chromeStarted(qint64 processId);
    void chromeEndpointReady(const QUrl &webSocketUrl);
    void connected();
    void disconnected();
    void commandResult(int id, const QJsonObject &result);
    void commandError(int id, const QString &message);
    void eventReceived(const QString &method, const QJsonObject &params,
                       const QString &sessionId);
    void errorOccurred(const QString &message);

private:
    void checkStartedChromeEndpoint();
    void requestWebSocketUrl(const QUrl &versionUrl, bool openSocketWhenReady);
    void handleTextMessage(const QString &message);

    QNetworkAccessManager *m_networkManager;
    QWebSocket *m_socket;
    QTimer *m_readyTimer;
    QUrl m_startupVersionUrl;
    int m_readyAttempts = 0;
    bool m_startupRequestInFlight = false;
    int m_nextCommandId = 1;
};

#endif // CDPCLIENT_H
