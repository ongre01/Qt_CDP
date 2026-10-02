#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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
    connect(ui->korailAutoLoginButton, &QPushButton::clicked,
            this, &MainWindow::startKorailAutoLogin);

    m_cdpReadyTimer.setInterval(200);
    connect(&m_cdpReadyTimer, &QTimer::timeout,
            this, &MainWindow::checkStartedChromeEndpoint);
    connect(&m_cdpSocket, &QWebSocket::connected,
            this, &MainWindow::onCdpSocketConnected);
    connect(&m_cdpSocket, &QWebSocket::textMessageReceived,
            this, &MainWindow::onCdpTextMessageReceived);
    connect(&m_cdpSocket, &QWebSocket::disconnected,
            this, &MainWindow::onCdpSocketDisconnected);

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

    if (!isLocalCdpHost(versionUrl.host())) {
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

void MainWindow::startKorailAutoLogin()
{
    const QString memberNumber = ui->memberNumberEdit->text().trimmed();
    const QString password = ui->korailPasswordEdit->text();
    QString errorMessage;
    const QUrl versionUrl = debuggerVersionUrl(&errorMessage);

    if (memberNumber.isEmpty() || password.isEmpty()) {
        showStatus(tr("코레일 회원번호와 비밀번호를 모두 입력하세요."), true);
        return;
    }
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    if (!isLocalCdpHost(versionUrl.host())) {
        showStatus(tr("자동 로그인은 로컬 CDP 주소에서만 실행할 수 있습니다."), true);
        return;
    }
    if (m_korailLoginInProgress) {
        return;
    }

    m_korailLoginInProgress = true;
    m_korailLoginStep = KorailLoginStep::Idle;
    m_pendingCdpCommandId = 0;
    m_korailFormCheckAttempts = 0;
    m_korailResultCheckAttempts = 0;
    m_cdpSessionId.clear();
    setBusy(true);
    showStatus(tr("코레일 로그인용 Chrome 탭에 연결하는 중입니다..."));

    QNetworkReply *reply = m_networkManager.get(QNetworkRequest(versionUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const bool requestSucceeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QString webSocketDebuggerUrl = document.object()
                                                .value(QStringLiteral("webSocketDebuggerUrl"))
                                                .toString();
        const QUrl webSocketUrl(webSocketDebuggerUrl);
        if (!m_korailLoginInProgress) {
            return;
        }
        if (!requestSucceeded || !webSocketUrl.isValid()
            || (webSocketUrl.scheme() != QStringLiteral("ws")
                && webSocketUrl.scheme() != QStringLiteral("wss"))) {
            finishKorailLogin(tr("CDP Chrome에 연결하지 못했습니다. 먼저 CDP용 Chrome을 시작하세요."), true);
            return;
        }

        m_cdpSocket.open(webSocketUrl);
    });
}

void MainWindow::onCdpSocketConnected()
{
    if (!m_korailLoginInProgress) {
        m_cdpSocket.close();
        return;
    }

    m_korailLoginStep = KorailLoginStep::CreatingTarget;
    sendCdpCommand(QStringLiteral("Target.createTarget"),
                   {{QStringLiteral("url"), QStringLiteral("https://www.korail.com/ticket/login")}});
}

void MainWindow::onCdpTextMessageReceived(const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    const QJsonObject response = document.object();
    if (response.isEmpty() || !m_korailLoginInProgress) {
        return;
    }

    if (!response.contains(QStringLiteral("id"))) {
        return;
    }
    if (response.value(QStringLiteral("id")).toInt() != m_pendingCdpCommandId) {
        return;
    }

    const QJsonObject error = response.value(QStringLiteral("error")).toObject();
    if (!error.isEmpty()) {
        finishKorailLogin(tr("Chrome CDP 명령을 실행하지 못했습니다: %1")
                               .arg(error.value(QStringLiteral("message")).toString()),
                           true);
        return;
    }

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    if (result.contains(QStringLiteral("exceptionDetails"))) {
        finishKorailLogin(tr("코레일 로그인 페이지에서 자동화 스크립트를 실행하지 못했습니다."), true);
        return;
    }

    switch (m_korailLoginStep) {
    case KorailLoginStep::CreatingTarget: {
        const QString targetId = result.value(QStringLiteral("targetId")).toString();
        if (targetId.isEmpty()) {
            finishKorailLogin(tr("코레일 로그인용 Chrome 탭을 만들지 못했습니다."), true);
            return;
        }
        m_korailLoginStep = KorailLoginStep::AttachingTarget;
        sendCdpCommand(QStringLiteral("Target.attachToTarget"),
                       {{QStringLiteral("targetId"), targetId},
                        {QStringLiteral("flatten"), true}});
        return;
    }
    case KorailLoginStep::AttachingTarget:
        m_cdpSessionId = result.value(QStringLiteral("sessionId")).toString();
        if (m_cdpSessionId.isEmpty()) {
            finishKorailLogin(tr("코레일 로그인 탭에 연결하지 못했습니다."), true);
            return;
        }
        m_korailLoginStep = KorailLoginStep::EnablingPage;
        sendCdpCommand(QStringLiteral("Page.enable"));
        return;
    case KorailLoginStep::EnablingPage:
        m_korailLoginStep = KorailLoginStep::Navigating;
        sendCdpCommand(QStringLiteral("Page.navigate"),
                       {{QStringLiteral("url"), QStringLiteral("https://www.korail.com/ticket/login")}});
        return;
    case KorailLoginStep::Navigating:
        m_korailFormCheckAttempts = 0;
        QTimer::singleShot(200, this, &MainWindow::waitForKorailLoginForm);
        return;
    case KorailLoginStep::WaitingForLoginForm: {
        const QJsonValue value = result.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("value"));
        if (value.toBool()) {
            submitKorailLogin();
            return;
        }
        if (++m_korailFormCheckAttempts >= 50) {
            finishKorailLogin(tr("코레일 로그인 페이지의 입력란을 찾지 못했습니다."), true);
            return;
        }
        QTimer::singleShot(200, this, &MainWindow::waitForKorailLoginForm);
        return;
    }
    case KorailLoginStep::SubmittingLogin:
        m_korailResultCheckAttempts = 0;
        QTimer::singleShot(500, this, &MainWindow::checkKorailLoginResult);
        return;
    case KorailLoginStep::CheckingLogin: {
        const QJsonObject value = result.value(QStringLiteral("result"))
                                      .toObject()
                                      .value(QStringLiteral("value"))
                                      .toObject();
        const bool loggedIn = value.value(QStringLiteral("loggedIn")).toBool();
        const bool loginFormPresent = value.value(QStringLiteral("loginFormPresent")).toBool();
        if (loggedIn || !loginFormPresent) {
            finishKorailLogin(tr("코레일 로그인 완료를 감지했습니다."));
            return;
        }
        if (++m_korailResultCheckAttempts >= 20) {
            finishKorailLogin(tr("로그인 완료를 확인하지 못했습니다. 열린 Chrome 탭을 확인하세요."), true);
            return;
        }
        QTimer::singleShot(500, this, &MainWindow::checkKorailLoginResult);
        return;
    }
    case KorailLoginStep::Idle:
        return;
    }
}

void MainWindow::onCdpSocketDisconnected()
{
    if (m_korailLoginInProgress) {
        finishKorailLogin(tr("Chrome CDP 연결이 끊어졌습니다."), true);
    }
}

void MainWindow::sendCdpCommand(const QString &method, const QJsonObject &parameters)
{
    QJsonObject command {
        {QStringLiteral("id"), m_nextCdpCommandId++},
        {QStringLiteral("method"), method}
    };
    if (!parameters.isEmpty()) {
        command.insert(QStringLiteral("params"), parameters);
    }
    if (!m_cdpSessionId.isEmpty()) {
        command.insert(QStringLiteral("sessionId"), m_cdpSessionId);
    }

    m_pendingCdpCommandId = command.value(QStringLiteral("id")).toInt();
    m_cdpSocket.sendTextMessage(QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact)));
}

void MainWindow::waitForKorailLoginForm()
{
    if (!m_korailLoginInProgress || m_cdpSessionId.isEmpty()) {
        return;
    }

    m_korailLoginStep = KorailLoginStep::WaitingForLoginForm;
    sendCdpCommand(QStringLiteral("Runtime.evaluate"),
                   {{QStringLiteral("expression"),
                     QStringLiteral("Boolean(document.querySelector('#id') && document.querySelector('#password') && document.querySelector('#tab_memNum .btn_bn-depblue'))")},
                    {QStringLiteral("returnByValue"), true}});
}

void MainWindow::submitKorailLogin()
{
    if (!m_korailLoginInProgress) {
        return;
    }

    const QJsonObject credentials {
        {QStringLiteral("memberNumber"), ui->memberNumberEdit->text().trimmed()},
        {QStringLiteral("password"), ui->korailPasswordEdit->text()}
    };
    const QString expression = QStringLiteral(R"JS(
(() => {
    const credentials = %1;
    const setValue = (element, value) => {
        const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
        setter.call(element, value);
        element.dispatchEvent(new Event('input', { bubbles: true }));
        element.dispatchEvent(new Event('change', { bubbles: true }));
    };
    const memberNumberInput = document.querySelector('#id');
    const passwordInput = document.querySelector('#password');
    const loginButton = document.querySelector('#tab_memNum .btn_bn-depblue');
    if (!memberNumberInput || !passwordInput || !loginButton) {
        throw new Error('Korail login controls are unavailable.');
    }
    setValue(memberNumberInput, credentials.memberNumber);
    setValue(passwordInput, credentials.password);
    loginButton.click();
    return true;
})()
)JS")
                                   .arg(QString::fromUtf8(QJsonDocument(credentials)
                                                              .toJson(QJsonDocument::Compact)));

    m_korailLoginStep = KorailLoginStep::SubmittingLogin;
    sendCdpCommand(QStringLiteral("Runtime.evaluate"),
                   {{QStringLiteral("expression"), expression},
                    {QStringLiteral("awaitPromise"), true},
                    {QStringLiteral("returnByValue"), true},
                    {QStringLiteral("userGesture"), true}});
}

void MainWindow::checkKorailLoginResult()
{
    if (!m_korailLoginInProgress || m_cdpSessionId.isEmpty()) {
        return;
    }

    m_korailLoginStep = KorailLoginStep::CheckingLogin;
    sendCdpCommand(QStringLiteral("Runtime.evaluate"),
                   {{QStringLiteral("expression"),
                     QStringLiteral("(() => ({ loginFormPresent: Boolean(document.querySelector('#id') && document.querySelector('#password')), loggedIn: Array.from(document.querySelectorAll('a, button')).some((element) => (element.textContent || '').trim() === '로그아웃') }))()")},
                    {QStringLiteral("returnByValue"), true}});
}

void MainWindow::finishKorailLogin(const QString &message, bool isError)
{
    m_korailLoginInProgress = false;
    m_korailLoginStep = KorailLoginStep::Idle;
    m_pendingCdpCommandId = 0;
    m_cdpSessionId.clear();
    ui->korailPasswordEdit->clear();
    if (m_cdpSocket.state() != QAbstractSocket::UnconnectedState) {
        m_cdpSocket.close();
    }
    setBusy(false);
    showStatus(message, isError);
}

void MainWindow::setBusy(bool busy)
{
    ui->startChromeButton->setEnabled(!busy);
    ui->chromeExecutableEdit->setEnabled(!busy);
    ui->userDataDirEdit->setEnabled(!busy);
    ui->debuggerEndpointEdit->setEnabled(!busy);
    ui->memberNumberEdit->setEnabled(!busy);
    ui->korailPasswordEdit->setEnabled(!busy);
    ui->korailAutoLoginButton->setEnabled(!busy);
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

bool MainWindow::isLocalCdpHost(const QString &host)
{
    const QString normalizedHost = host.toLower();
    return normalizedHost == QStringLiteral("127.0.0.1")
           || normalizedHost == QStringLiteral("localhost")
           || normalizedHost == QStringLiteral("::1");
}
