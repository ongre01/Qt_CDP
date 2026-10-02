#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->startChromeButton, &QPushButton::clicked,
            this, &MainWindow::startChromeForCdp);

    m_cdpReadyTimer.setInterval(200);
    connect(&m_cdpReadyTimer, &QTimer::timeout,
            this, &MainWindow::checkStartedChromeEndpoint);

}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::startChromeForCdp()
{
    const QString chromeExecutable = ui->chromeExecutableEdit->text().trimmed();
    const QString userDataDirectory = ui->userDataDirEdit->text().trimmed();
    QString errorMessage;
    const QUrl versionUrl = debuggerVersionUrl(&errorMessage);

    if (chromeExecutable.isEmpty() || !QFileInfo::exists(chromeExecutable)) {
        showStatus(tr("Chrome 실행 파일을 찾을 수 없습니다."), true);
        return;
    }
    if (userDataDirectory.isEmpty()) {
        showStatus(tr("CDP 전용 사용자 데이터 디렉터리를 입력하세요."), true);
        return;
    }
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }

    const QString host = versionUrl.host().toLower();
    if (host != QStringLiteral("127.0.0.1")
        && host != QStringLiteral("localhost")
        && host != QStringLiteral("::1")) {
        showStatus(tr("Chrome 시작은 로컬 CDP 주소만 지원합니다."), true);
        return;
    }

    const QStringList arguments {
        QStringLiteral("--remote-debugging-port=%1").arg(versionUrl.port()),
        QStringLiteral("--user-data-dir=%1").arg(QDir::cleanPath(userDataDirectory))
    };
    qint64 processId = 0;
    if (!QProcess::startDetached(chromeExecutable, arguments, QString(), &processId)) {
        showStatus(tr("Chrome을 시작하지 못했습니다."), true);
        return;
    }

    m_cdpReadyAttempts = 0;
    m_startupRequestInFlight = false;
    m_cdpReadyTimer.start();
    setBusy(true);
    showStatus(tr("CDP용 Chrome(PID: %1)을 시작했습니다. 디버거 연결을 기다리는 중입니다...")
                   .arg(processId));
}

void MainWindow::checkStartedChromeEndpoint()
{
    if (m_startupRequestInFlight) {
        return;
    }

    constexpr int maxAttempts = 50;
    if (++m_cdpReadyAttempts > maxAttempts) {
        m_cdpReadyTimer.stop();
        setBusy(false);
        showStatus(tr("Chrome CDP 엔드포인트가 10초 안에 준비되지 않았습니다."), true);
        return;
    }

    QString errorMessage;
    const QUrl versionUrl = debuggerVersionUrl(&errorMessage);
    if (!versionUrl.isValid()) {
        m_cdpReadyTimer.stop();
        setBusy(false);
        showStatus(errorMessage, true);
        return;
    }

    m_startupRequestInFlight = true;
    QNetworkReply *reply = m_networkManager.get(QNetworkRequest(versionUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const bool requestSucceeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_startupRequestInFlight = false;

        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QString webSocketDebuggerUrl = document.object()
                                                .value(QStringLiteral("webSocketDebuggerUrl"))
                                                .toString();
        if (!requestSucceeded || webSocketDebuggerUrl.isEmpty()) {
            return;
        }

        m_cdpReadyTimer.stop();
        setBusy(false);
        showStatus(tr("CDP용 Chrome이 준비되었습니다. Qt에서 CDP 제어를 시작할 수 있습니다."));
    });
}

void MainWindow::setBusy(bool busy)
{
    ui->startChromeButton->setEnabled(!busy);
    ui->chromeExecutableEdit->setEnabled(!busy);
    ui->userDataDirEdit->setEnabled(!busy);
    ui->debuggerEndpointEdit->setEnabled(!busy);
}

void MainWindow::showStatus(const QString &message, bool isError)
{
    ui->statusLabel->setText(message);
    ui->statusLabel->setStyleSheet(isError
                                       ? QStringLiteral("color: #b00020;")
                                       : QStringLiteral("color: #1b5e20;"));
}

QUrl MainWindow::debuggerVersionUrl(QString *errorMessage) const
{
    QString endpointText = ui->debuggerEndpointEdit->text().trimmed();
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
