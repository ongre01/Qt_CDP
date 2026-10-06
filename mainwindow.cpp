#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVector>

namespace {

QString normalizedText(const QString &text)
{
    QString result = text;
    result.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return result.trimmed();
}

QJsonArray trainInfoFromDomSnapshot(const QJsonObject &snapshot, bool *isTicketReservationPage)
{
    if (isTicketReservationPage) {
        *isTicketReservationPage = false;
    }

    const QJsonArray documents = snapshot.value(QStringLiteral("documents")).toArray();
    const QJsonArray strings = snapshot.value(QStringLiteral("strings")).toArray();
    if (documents.isEmpty() || strings.isEmpty()) {
        return {};
    }

    const QJsonObject nodes = documents.first().toObject().value(QStringLiteral("nodes")).toObject();
    const QJsonArray nodeNames = nodes.value(QStringLiteral("nodeName")).toArray();
    const QJsonArray nodeValues = nodes.value(QStringLiteral("nodeValue")).toArray();
    const QJsonArray parentIndexes = nodes.value(QStringLiteral("parentIndex")).toArray();
    const QJsonArray attributes = nodes.value(QStringLiteral("attributes")).toArray();
    if (nodeNames.isEmpty() || parentIndexes.size() != nodeNames.size()) {
        return {};
    }

    const auto stringAt = [&strings](int index) {
        return index >= 0 && index < strings.size() ? strings.at(index).toString() : QString();
    };
    const auto nodeName = [&nodeNames, &stringAt](int index) {
        return stringAt(nodeNames.at(index).toInt(-1)).toLower();
    };
    const auto attributeValue = [&attributes, &stringAt](int nodeIndex, const QString &attributeName) {
        if (nodeIndex < 0 || nodeIndex >= attributes.size()) {
            return QString();
        }
        const QJsonArray pairs = attributes.at(nodeIndex).toArray();
        for (int pairIndex = 0; pairIndex + 1 < pairs.size(); pairIndex += 2) {
            if (stringAt(pairs.at(pairIndex).toInt(-1)).compare(attributeName, Qt::CaseInsensitive) == 0) {
                return stringAt(pairs.at(pairIndex + 1).toInt(-1));
            }
        }
        return QString();
    };

    QVector<QVector<int>> children(nodeNames.size());
    for (int index = 0; index < parentIndexes.size(); ++index) {
        const int parentIndex = parentIndexes.at(index).toInt(-1);
        if (parentIndex >= 0 && parentIndex < children.size()) {
            children[parentIndex].append(index);
        }
    }

    const auto textForSubtree = [&children, &nodeName, &nodeValues, &stringAt](int root) {
        QStringList fragments;
        QVector<int> pending {root};
        while (!pending.isEmpty()) {
            const int index = pending.takeLast();
            if (nodeName(index) == QStringLiteral("#text")) {
                const int valueIndex = index < nodeValues.size()
                    ? nodeValues.at(index).toInt(-1)
                    : -1;
                fragments.append(stringAt(valueIndex));
            }
            const QVector<int> &childNodes = children.at(index);
            for (auto child = childNodes.crbegin(); child != childNodes.crend(); ++child) {
                pending.append(*child);
            }
        }
        return normalizedText(fragments.join(QLatin1Char(' ')));
    };
    const auto textForClassToken = [&children, &attributeValue, &textForSubtree](int root,
                                                                                   const QString &classToken) {
        QVector<int> pending {root};
        while (!pending.isEmpty()) {
            const int index = pending.takeLast();
            const QStringList classes = attributeValue(index, QStringLiteral("class"))
                                            .split(QRegularExpression(QStringLiteral("\\s+")),
                                                   Qt::SkipEmptyParts);
            if (classes.contains(classToken, Qt::CaseInsensitive)) {
                return textForSubtree(index);
            }
            const QVector<int> &childNodes = children.at(index);
            for (auto child = childNodes.crbegin(); child != childNodes.crend(); ++child) {
                pending.append(*child);
            }
        }
        return QString();
    };

    int bodyIndex = -1;
    for (int index = 0; index < nodeNames.size(); ++index) {
        if (nodeName(index) == QStringLiteral("body")) {
            bodyIndex = index;
            break;
        }
    }
    const QString pageText = bodyIndex >= 0 ? textForSubtree(bodyIndex) : textForSubtree(0);
    const bool reservationPage = QRegularExpression(
        QStringLiteral("승차권\\s*예매|열차\\s*(조회|예매)|출발역\\s*.*도착역"))
                                     .match(pageText)
                                     .hasMatch();
    if (isTicketReservationPage) {
        *isTicketReservationPage = reservationPage;
    }
    if (!reservationPage) {
        return {};
    }

    const QRegularExpression timePattern(QStringLiteral("(?:[01]?\\d|2[0-3]):[0-5]\\d"));
    const QRegularExpression trainTypePattern(
        QStringLiteral("KTX(?:-산천)?|SRT|ITX-?(?:새마을|마음)|새마을호|무궁화호|누리로|통근열차"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression trainNumberPattern(QStringLiteral("(?:열차\\s*번호\\s*)?(\\d{3,5})\\s*호?"));
    const QRegularExpression stationPairPattern(
        QStringLiteral("([가-힣A-Za-z0-9]+)\\s*(?:역)?\\s*(?:→|->|~|-)\\s*([가-힣A-Za-z0-9]+)\\s*(?:역)?"));
    const QRegularExpression availabilityPattern(QStringLiteral("예매|예약|좌석|매진|입석|잔여|특실|일반실"));
    QSet<QString> seenRows;
    QJsonArray trains;

    for (int index = 0; index < nodeNames.size() && trains.size() < 200; ++index) {
        const QString tagName = nodeName(index);
        const bool isTrainListItem = tagName == QStringLiteral("li")
            && attributeValue(index, QStringLiteral("class"))
                   .contains(QStringLiteral("tckList"), Qt::CaseInsensitive);
        if (tagName != QStringLiteral("tr") && !isTrainListItem) {
            continue;
        }

        const QString rowText = textForSubtree(index);
        const QString titleText = isTrainListItem
            ? textForClassToken(index, QStringLiteral("tit_box"))
            : rowText;
        const QString travelText = isTrainListItem
            ? textForClassToken(index, QStringLiteral("data_box"))
            : rowText;
        const QString generalSeat = isTrainListItem
            ? textForClassToken(index, QStringLiteral("gen"))
            : rowText;
        const QString specialSeat = isTrainListItem
            ? textForClassToken(index, QStringLiteral("spe"))
            : QString();
        QRegularExpressionMatchIterator timeMatches = timePattern.globalMatch(travelText);
        QStringList times;
        while (timeMatches.hasNext()) {
            times.append(timeMatches.next().captured(0));
        }
        if (times.size() < 2 || !availabilityPattern.match(rowText).hasMatch()) {
            continue;
        }

        const QRegularExpressionMatch typeMatch = trainTypePattern.match(titleText);
        const QRegularExpressionMatch numberMatch = trainNumberPattern.match(titleText);
        const QRegularExpressionMatch stationMatch = stationPairPattern.match(travelText);
        const QRegularExpressionMatch durationMatch = QRegularExpression(
            QStringLiteral("소요시간\\s*:\\s*(.+)$")).match(travelText);
        const QString trainType = typeMatch.hasMatch() ? typeMatch.captured(0) : QString();
        const QString trainNumber = numberMatch.hasMatch() ? numberMatch.captured(1) : QString();
        const QString departure = stationMatch.hasMatch() ? stationMatch.captured(1) : QString();
        const QString arrival = stationMatch.hasMatch() ? stationMatch.captured(2) : QString();
        const QString duration = durationMatch.hasMatch() ? durationMatch.captured(1).trimmed() : QString();
        const QString signature = QStringList {trainType, trainNumber, departure, times.at(0),
                                               arrival, times.at(1), duration, generalSeat, specialSeat}
                                      .join(QLatin1Char('|'));
        if (seenRows.contains(signature)) {
            continue;
        }
        seenRows.insert(signature);
        trains.append(QJsonObject {
            {QStringLiteral("trainType"), trainType},
            {QStringLiteral("trainNumber"), trainNumber},
            {QStringLiteral("departure"), departure},
            {QStringLiteral("departureTime"), times.at(0)},
            {QStringLiteral("arrival"), arrival},
            {QStringLiteral("arrivalTime"), times.at(1)},
            {QStringLiteral("duration"), duration},
            {QStringLiteral("generalSeat"), generalSeat},
            {QStringLiteral("specialSeat"), specialSeat}
        });
    }
    return trains;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->startChromeButton, &QPushButton::clicked,
            this, &MainWindow::startChromeForCdp);
    connect(ui->korailAutoLoginButton, &QPushButton::clicked,
            this, &MainWindow::startKorailAutoLogin);
    connect(ui->pageRecordingButton, &QPushButton::clicked,
            this, &MainWindow::togglePageRecording);

    ui->snapshotDirectoryEdit->setText(defaultSnapshotDirectory());
    ui->trainInfoTableWidget->setColumnCount(9);
    ui->trainInfoTableWidget->setHorizontalHeaderLabels(
        {tr("열차"), tr("번호"), tr("출발역"), tr("출발 시각"),
         tr("도착역"), tr("도착 시각"), tr("소요 시간"), tr("일반실"), tr("특실")});
    ui->trainInfoTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->trainInfoTableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->trainInfoTableWidget->verticalHeader()->setVisible(false);
    ui->trainInfoTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->trainInfoTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->trainInfoGroupBox->setVisible(false);

    m_cdpReadyTimer.setInterval(200);
    connect(&m_cdpReadyTimer, &QTimer::timeout,
            this, &MainWindow::checkStartedChromeEndpoint);
    connect(&m_cdpSocket, &QWebSocket::connected,
            this, &MainWindow::onCdpSocketConnected);
    connect(&m_cdpSocket, &QWebSocket::textMessageReceived,
            this, &MainWindow::onCdpTextMessageReceived);
    connect(&m_cdpSocket, &QWebSocket::disconnected,
            this, &MainWindow::onCdpSocketDisconnected);
    connect(&m_recorderSocket, &QWebSocket::connected,
            this, &MainWindow::onRecorderSocketConnected);
    connect(&m_recorderSocket, &QWebSocket::textMessageReceived,
            this, &MainWindow::onRecorderTextMessageReceived);
    connect(&m_recorderSocket, &QWebSocket::disconnected,
            this, &MainWindow::onRecorderSocketDisconnected);

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

void MainWindow::togglePageRecording()
{
    if (m_pageRecordingRequested || m_pageRecordingActive) {
        stopPageRecording(tr("방문 페이지 기록을 중지했습니다."));
        return;
    }

    startPageRecording();
}

void MainWindow::startPageRecording()
{
    const QString snapshotDirectory = ui->snapshotDirectoryEdit->text().trimmed();
    QString errorMessage;
    const QUrl versionUrl = debuggerVersionUrl(&errorMessage);

    if (snapshotDirectory.isEmpty()) {
        showStatus(tr("스냅샷 저장 폴더를 입력하세요."), true);
        return;
    }
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    if (!isLocalCdpHost(versionUrl.host())) {
        showStatus(tr("방문 페이지 기록은 로컬 CDP 주소에서만 지원합니다."), true);
        return;
    }
    if (!QDir().mkpath(snapshotDirectory)) {
        showStatus(tr("스냅샷 저장 폴더를 만들 수 없습니다."), true);
        return;
    }

    m_pageRecordingRequested = true;
    ui->pageRecordingButton->setEnabled(false);
    showStatus(tr("방문 페이지 기록용 CDP 연결을 준비하는 중입니다..."));

    QNetworkReply *reply = m_networkManager.get(QNetworkRequest(versionUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const bool requestSucceeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QUrl webSocketUrl(document.object()
                                    .value(QStringLiteral("webSocketDebuggerUrl"))
                                    .toString());
        if (!m_pageRecordingRequested) {
            return;
        }
        if (!requestSucceeded || !webSocketUrl.isValid()
            || (webSocketUrl.scheme() != QStringLiteral("ws")
                && webSocketUrl.scheme() != QStringLiteral("wss"))) {
            m_pageRecordingRequested = false;
            ui->pageRecordingButton->setEnabled(true);
            showStatus(tr("페이지 기록용 CDP Chrome에 연결하지 못했습니다. 먼저 CDP용 Chrome을 시작하세요."),
                       true);
            return;
        }

        m_recorderSocket.open(webSocketUrl);
    });
}

void MainWindow::stopPageRecording(const QString &message)
{
    const bool wasRecording = m_pageRecordingRequested || m_pageRecordingActive;
    m_pageRecordingRequested = false;
    m_pageRecordingActive = false;
    m_recorderRequests.clear();
    m_recorderSessions.clear();
    m_targetToRecorderSession.clear();
    clearTrainInfoTable();
    ui->pageRecordingButton->setEnabled(true);
    ui->pageRecordingButton->setText(tr("방문 페이지 기록 시작"));

    if (m_recorderSocket.state() != QAbstractSocket::UnconnectedState) {
        m_recorderSocket.close();
    }
    if (wasRecording && !message.isEmpty()) {
        showStatus(message);
    }
}

void MainWindow::onRecorderSocketConnected()
{
    if (!m_pageRecordingRequested) {
        m_recorderSocket.close();
        return;
    }

    m_pageRecordingActive = true;
    ui->pageRecordingButton->setEnabled(true);
    ui->pageRecordingButton->setText(tr("방문 페이지 기록 중지"));

    sendRecorderCommand(QStringLiteral("Target.setDiscoverTargets"),
                        {{QStringLiteral("discover"), true}});
    sendRecorderCommand(QStringLiteral("Target.setAutoAttach"),
                        {{QStringLiteral("autoAttach"), true},
                         {QStringLiteral("waitForDebuggerOnStart"), false},
                         {QStringLiteral("flatten"), true},
                         {QStringLiteral("filter"),
                          QJsonArray {QJsonObject {{QStringLiteral("type"), QStringLiteral("page")}}}}});

    const int commandId = sendRecorderCommand(QStringLiteral("Target.getTargets"));
    if (commandId != 0) {
        m_recorderRequests.insert(commandId, {RecorderRequestType::TargetList, {}, {}, {}});
    }

    showStatus(tr("방문 페이지 기록 중입니다. 입력값과 textarea 값은 저장하지 않습니다."));
}

void MainWindow::onRecorderTextMessageReceived(const QString &message)
{
    const QJsonObject response = QJsonDocument::fromJson(message.toUtf8()).object();
    if (response.isEmpty()) {
        return;
    }

    if (!response.contains(QStringLiteral("id"))) {
        handleRecorderEvent(response);
        return;
    }

    const int commandId = response.value(QStringLiteral("id")).toInt();
    if (!m_recorderRequests.contains(commandId)) {
        return;
    }

    const RecorderRequest request = m_recorderRequests.take(commandId);
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const bool failed = !response.value(QStringLiteral("error")).toObject().isEmpty()
                        || !result.value(QStringLiteral("exceptionDetails")).toObject().isEmpty();
    if (failed) {
        if (request.type == RecorderRequestType::DomSnapshot
            && m_recorderSessions.contains(request.sessionId)) {
            m_recorderSessions[request.sessionId].captureInFlight = false;
        }
        return;
    }

    switch (request.type) {
    case RecorderRequestType::TargetList: {
        const QJsonArray targetInfos = result.value(QStringLiteral("targetInfos")).toArray();
        for (const QJsonValue &value : targetInfos) {
            const QJsonObject targetInfo = value.toObject();
            if (targetInfo.value(QStringLiteral("type")).toString() != QStringLiteral("page")) {
                continue;
            }

            const QString targetId = targetInfo.value(QStringLiteral("targetId")).toString();
            if (!targetId.isEmpty() && !m_targetToRecorderSession.contains(targetId)) {
                sendRecorderCommand(QStringLiteral("Target.attachToTarget"),
                                    {{QStringLiteral("targetId"), targetId},
                                     {QStringLiteral("flatten"), true}});
            }
        }
        return;
    }
    case RecorderRequestType::DomSnapshot:
        saveDomSnapshot(request, result);
        return;
    }
}

void MainWindow::onRecorderSocketDisconnected()
{
    const bool wasRecording = m_pageRecordingRequested || m_pageRecordingActive;
    m_pageRecordingActive = false;
    m_pageRecordingRequested = false;
    m_recorderRequests.clear();
    m_recorderSessions.clear();
    m_targetToRecorderSession.clear();
    clearTrainInfoTable();
    ui->pageRecordingButton->setEnabled(true);
    ui->pageRecordingButton->setText(tr("방문 페이지 기록 시작"));

    if (wasRecording) {
        showStatus(tr("페이지 기록용 CDP 연결이 끊어졌습니다."), true);
    }
}

int MainWindow::sendRecorderCommand(const QString &method,
                                    const QJsonObject &parameters,
                                    const QString &sessionId)
{
    if (m_recorderSocket.state() != QAbstractSocket::ConnectedState) {
        return 0;
    }

    const int commandId = m_nextRecorderCommandId++;
    QJsonObject command {
        {QStringLiteral("id"), commandId},
        {QStringLiteral("method"), method}
    };
    if (!parameters.isEmpty()) {
        command.insert(QStringLiteral("params"), parameters);
    }
    if (!sessionId.isEmpty()) {
        command.insert(QStringLiteral("sessionId"), sessionId);
    }

    m_recorderSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact)));
    return commandId;
}

void MainWindow::handleRecorderEvent(const QJsonObject &event)
{
    if (!m_pageRecordingActive) {
        return;
    }

    const QString method = event.value(QStringLiteral("method")).toString();
    const QJsonObject parameters = event.value(QStringLiteral("params")).toObject();
    if (method == QStringLiteral("Target.attachedToTarget")) {
        attachRecorderToPage(parameters);
        return;
    }
    if (method == QStringLiteral("Target.detachedFromTarget")) {
        const QString sessionId = parameters.value(QStringLiteral("sessionId")).toString();
        const bool wasTrainInfoSession = sessionId == m_trainInfoSessionId;
        if (m_recorderSessions.contains(sessionId)) {
            m_targetToRecorderSession.remove(m_recorderSessions.value(sessionId).targetId);
            m_recorderSessions.remove(sessionId);
        }
        if (wasTrainInfoSession) {
            clearTrainInfoTable();
        }
        return;
    }
    if (method == QStringLiteral("Target.targetInfoChanged")) {
        const QJsonObject targetInfo = parameters.value(QStringLiteral("targetInfo")).toObject();
        const QString targetId = targetInfo.value(QStringLiteral("targetId")).toString();
        const QString sessionId = m_targetToRecorderSession.value(targetId);
        if (!sessionId.isEmpty() && m_recorderSessions.contains(sessionId)) {
            RecorderSession &session = m_recorderSessions[sessionId];
            session.url = targetInfo.value(QStringLiteral("url")).toString();
            session.title = targetInfo.value(QStringLiteral("title")).toString();
        }
        return;
    }
    if (method == QStringLiteral("Network.loadingFinished")) {
        schedulePageSnapshot(event.value(QStringLiteral("sessionId")).toString(), 700);
        return;
    }
    if (method == QStringLiteral("Page.loadEventFired")
        || method == QStringLiteral("Page.navigatedWithinDocument")) {
        schedulePageSnapshot(event.value(QStringLiteral("sessionId")).toString());
    }
}

void MainWindow::attachRecorderToPage(const QJsonObject &parameters)
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
    if (m_targetToRecorderSession.contains(targetId)) {
        sendRecorderCommand(QStringLiteral("Target.detachFromTarget"),
                            {{QStringLiteral("sessionId"), sessionId}});
        return;
    }

    m_recorderSessions.insert(sessionId,
                              {targetId,
                               targetInfo.value(QStringLiteral("url")).toString(),
                               targetInfo.value(QStringLiteral("title")).toString(),
                               false,
                               false,
                               {}});
    m_targetToRecorderSession.insert(targetId, sessionId);

    sendRecorderCommand(QStringLiteral("Page.enable"), {}, sessionId);
    sendRecorderCommand(QStringLiteral("Network.enable"), {}, sessionId);
    schedulePageSnapshot(sessionId, 800);
}

void MainWindow::schedulePageSnapshot(const QString &sessionId, int delayMilliseconds)
{
    if (!m_pageRecordingActive || !m_recorderSessions.contains(sessionId)) {
        return;
    }

    RecorderSession &session = m_recorderSessions[sessionId];
    if (session.captureScheduled || session.captureInFlight) {
        return;
    }
    if (!isRecordablePageUrl(session.url)) {
        return;
    }

    session.captureScheduled = true;
    QTimer::singleShot(delayMilliseconds, this, [this, sessionId]() {
        if (!m_pageRecordingActive || !m_recorderSessions.contains(sessionId)) {
            return;
        }

        m_recorderSessions[sessionId].captureScheduled = false;
        capturePageSnapshot(sessionId);
    });
}

void MainWindow::capturePageSnapshot(const QString &sessionId)
{
    if (!m_pageRecordingActive || !m_recorderSessions.contains(sessionId)) {
        return;
    }

    RecorderSession &session = m_recorderSessions[sessionId];
    if (session.captureInFlight) {
        return;
    }

    const QString snapshotDirectory = ui->snapshotDirectoryEdit->text().trimmed();
    const QString captureId = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"))
                              + QStringLiteral("-%1").arg(m_nextSnapshotSequence++);
    const QString captureDirectory = QDir(snapshotDirectory).filePath(captureId);
    if (!QDir().mkpath(captureDirectory)) {
        showStatus(tr("페이지 스냅샷 폴더를 만들 수 없습니다."), true);
        return;
    }

    session.captureInFlight = true;
    const int domSnapshotCommandId = sendRecorderCommand(QStringLiteral("DOMSnapshot.captureSnapshot"),
                                                         {{QStringLiteral("computedStyles"), QJsonArray {}},
                                                          {QStringLiteral("includeDOMRects"), true}},
                                                         sessionId);
    if (domSnapshotCommandId == 0) {
        session.captureInFlight = false;
        return;
    }
    m_recorderRequests.insert(domSnapshotCommandId,
                              {RecorderRequestType::DomSnapshot, captureId, captureDirectory, sessionId});
    return;

#if 0 // Replaced by the passive DOMSnapshot path above; retained temporarily for source comparison.
    static const QString captureExpression = QStringLiteral(R"JS(
(() => {
    const redacted = '[REDACTED]';
    const redactUrl = (rawUrl) => {
        try {
            const url = new URL(rawUrl);
            for (const key of Array.from(url.searchParams.keys())) {
                if (/(pass(word)?|secret|token|auth|session|cookie|card|cvv|ssn)/i.test(key)) {
                    url.searchParams.set(key, redacted);
                }
            }
            return url.href;
        } catch (_) {
            return rawUrl;
        }
    };
    const isSensitive = (element) => {
        const identity = [
            element.getAttribute('type'),
            element.getAttribute('name'),
            element.getAttribute('id'),
            element.getAttribute('autocomplete')
        ].filter(Boolean).join(' ');
        return /(pass(word)?|secret|token|auth|session|cookie|card|cvv|ssn)/i.test(identity);
    };
    const cssEscape = window.CSS && CSS.escape
        ? CSS.escape
        : (value) => String(value).replace(/[^a-zA-Z0-9_-]/g, '\\$&');
    const selectorFor = (element) => {
        if (element.id) {
            const candidate = `#${cssEscape(element.id)}`;
            if (document.querySelectorAll(candidate).length === 1) {
                return candidate;
            }
        }
        for (const attribute of ['data-testid', 'data-test', 'data-qa', 'name', 'aria-label']) {
            const value = element.getAttribute(attribute);
            if (!value) {
                continue;
            }
            const candidate = `${element.tagName.toLowerCase()}[${attribute}="${cssEscape(value)}"]`;
            if (document.querySelectorAll(candidate).length === 1) {
                return candidate;
            }
        }
        const parts = [];
        let current = element;
        while (current && current.nodeType === Node.ELEMENT_NODE && parts.length < 8) {
            let index = 1;
            let sibling = current.previousElementSibling;
            while (sibling) {
                if (sibling.tagName === current.tagName) {
                    ++index;
                }
                sibling = sibling.previousElementSibling;
            }
            parts.unshift(`${current.tagName.toLowerCase()}:nth-of-type(${index})`);
            current = current.parentElement;
        }
        return parts.join(' > ');
    };
    const rootClone = document.documentElement.cloneNode(true);
    const originals = document.querySelectorAll('input, textarea, select');
    const clones = rootClone.querySelectorAll('input, textarea, select');
    originals.forEach((element, index) => {
        const clone = clones[index];
        if (!clone) {
            return;
        }
        clone.removeAttribute('value');
        if (clone.tagName === 'TEXTAREA') {
            clone.textContent = '';
        }
        if (isSensitive(element)) {
            clone.setAttribute('data-cdp-value-redacted', 'true');
        }
    });
    const actionableElements = Array.from(document.querySelectorAll(
        'a, button, input, textarea, select, [role="button"], [role="link"], [contenteditable="true"]'
    )).slice(0, 5000).map((element) => {
        const rect = element.getBoundingClientRect();
        const input = element instanceof HTMLInputElement ? element : null;
        return {
            selector: selectorFor(element),
            tag: element.tagName.toLowerCase(),
            id: element.id || '',
            name: element.getAttribute('name') || '',
            type: input ? input.type : '',
            role: element.getAttribute('role') || '',
            text: isSensitive(element) ? '' : (element.innerText || element.textContent || '').trim().slice(0, 500),
            ariaLabel: element.getAttribute('aria-label') || '',
            placeholder: element.getAttribute('placeholder') || '',
            title: element.getAttribute('title') || '',
            href: element instanceof HTMLAnchorElement ? redactUrl(element.href) : '',
            checked: input ? input.checked : false,
            visible: Boolean(rect.width || rect.height),
            rect: { x: rect.x, y: rect.y, width: rect.width, height: rect.height }
        };
    });

    const cleanText = (value) => String(value || '').replace(/\s+/g, ' ').trim();
    const bodyText = cleanText(document.body && document.body.innerText);
    const ticketReservationPage = /승차권\s*예매|열차\s*(조회|예매)|출발역\s*.*도착역/.test(bodyText);
    const trains = [];
    const seenRows = new Set();
    const textFor = (element) => cleanText(element && (element.innerText || element.textContent));
    const cellFor = (headers, values, names) => {
        const index = headers.findIndex((header) => names.some((name) => header.includes(name)));
        return index >= 0 ? (values[index] || '') : '';
    };
    const addTrainRow = (row, headers = []) => {
        if (trains.length >= 200) {
            return;
        }
        const cells = Array.from(row.querySelectorAll(':scope > th, :scope > td'));
        const values = cells.map(textFor).filter(Boolean);
        const rowText = cleanText(values.join(' '));
        const times = rowText.match(/(?:[01]?\d|2[0-3]):[0-5]\d/g) || [];
        const hasBookingInfo = /예매|예약|좌석|매진|입석|잔여|특실|일반실/.test(rowText);
        if (times.length < 2 || !hasBookingInfo) {
            return;
        }

        const normalizedHeaders = headers.map(cleanText);
        const trainType = cellFor(normalizedHeaders, values, ['열차종류', '열차명', '열차'])
            || (rowText.match(/KTX(?:-산천)?|SRT|ITX-?(?:새마을|마음)|새마을호|무궁화호|누리로|통근열차/i) || [''])[0];
        const trainNumber = row.getAttribute('data-train-no')
            || row.getAttribute('data-trainno')
            || row.getAttribute('data-train-number')
            || cellFor(normalizedHeaders, values, ['열차번호', '번호'])
            || (rowText.match(/(?:열차\s*번호\s*)?(\d{3,5})\s*호?/) || ['', ''])[1];
        const stationPair = rowText.match(/([가-힣A-Za-z0-9]+)\s*(?:역)?\s*(?:→|->|~|-)\s*([가-힣A-Za-z0-9]+)\s*(?:역)?/);
        const departure = cellFor(normalizedHeaders, values, ['출발역', '출발지'])
            || (stationPair ? stationPair[1] : '');
        const arrival = cellFor(normalizedHeaders, values, ['도착역', '도착지'])
            || (stationPair ? stationPair[2] : '');
        const departureTime = cellFor(normalizedHeaders, values, ['출발시간', '출발 시각']) || times[0];
        const arrivalTime = cellFor(normalizedHeaders, values, ['도착시간', '도착 시각']) || times[1];
        const availability = values.filter((value) => /예매|예약|좌석|매진|입석|잔여|특실|일반실/.test(value))
            .join(' / ');
        const signature = [trainType, trainNumber, departure, departureTime, arrival, arrivalTime, availability]
            .join('|');
        if (seenRows.has(signature)) {
            return;
        }
        seenRows.add(signature);
        trains.push({ trainType, trainNumber, departure, departureTime, arrival, arrivalTime, availability });
    };

    Array.from(document.querySelectorAll('table')).forEach((table) => {
        const headers = Array.from(table.querySelectorAll('thead th')).map(textFor);
        Array.from(table.querySelectorAll('tbody tr')).forEach((row) => addTrainRow(row, headers));
    });
    Array.from(document.querySelectorAll('[data-train-no], [data-trainno], [data-train-number]'))
        .forEach((element) => addTrainRow(element.closest('tr') || element));

    return {
        capturedAt: new Date().toISOString(),
        url: redactUrl(location.href),
        title: document.title,
        readyState: document.readyState,
        viewport: { width: window.innerWidth, height: window.innerHeight, devicePixelRatio: window.devicePixelRatio },
        ticketReservationPage,
        trains,
        documentHtml: '<!DOCTYPE html>\n' + rootClone.outerHTML,
        actionableElements
    };
})()
)JS");

    session.captureInFlight = true;
    const int pageCommandId = sendRecorderCommand(QStringLiteral("Runtime.evaluate"),
                                                  {{QStringLiteral("expression"), captureExpression},
                                                   {QStringLiteral("returnByValue"), true},
                                                   {QStringLiteral("awaitPromise"), true}},
                                                  sessionId);
    if (pageCommandId == 0) {
        session.captureInFlight = false;
        return;
    }
    m_recorderRequests.insert(pageCommandId,
                              {RecorderRequestType::PageData, captureId, captureDirectory, sessionId});

    const int domCommandId = sendRecorderCommand(QStringLiteral("DOMSnapshot.captureSnapshot"),
                                                 {{QStringLiteral("computedStyles"), QJsonArray {}},
                                                  {QStringLiteral("includeDOMRects"), true}},
                                                 sessionId);
    if (domCommandId != 0) {
        m_recorderRequests.insert(domCommandId,
                                  {RecorderRequestType::DomSnapshot, captureId, captureDirectory, sessionId});
    }
#endif
}

void MainWindow::updateTrainInfoTable(const QJsonArray &trains)
{
    ui->trainInfoTableWidget->setUpdatesEnabled(false);
    ui->trainInfoTableWidget->setRowCount(0);

    for (const QJsonValue &value : trains) {
        const QJsonObject train = value.toObject();
        const int row = ui->trainInfoTableWidget->rowCount();
        ui->trainInfoTableWidget->insertRow(row);
        const QStringList columns {
            train.value(QStringLiteral("trainType")).toString(),
            train.value(QStringLiteral("trainNumber")).toString(),
            train.value(QStringLiteral("departure")).toString(),
            train.value(QStringLiteral("departureTime")).toString(),
            train.value(QStringLiteral("arrival")).toString(),
            train.value(QStringLiteral("arrivalTime")).toString(),
            train.value(QStringLiteral("duration")).toString(),
            train.value(QStringLiteral("generalSeat")).toString(),
            train.value(QStringLiteral("specialSeat")).toString()
        };
        for (int column = 0; column < columns.size(); ++column) {
            auto *item = new QTableWidgetItem(columns.at(column));
            item->setToolTip(columns.at(column));
            ui->trainInfoTableWidget->setItem(row, column, item);
        }
    }

    ui->trainInfoTableWidget->setUpdatesEnabled(true);
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (%1건)").arg(trains.size()));
    ui->trainInfoGroupBox->setVisible(true);
}

void MainWindow::clearTrainInfoTable()
{
    m_trainInfoSessionId.clear();
    ui->trainInfoTableWidget->setRowCount(0);
    ui->trainInfoGroupBox->setVisible(false);
}

void MainWindow::saveDomSnapshot(const RecorderRequest &request, const QJsonObject &result)
{
    if (m_recorderSessions.contains(request.sessionId)) {
        m_recorderSessions[request.sessionId].captureInFlight = false;
    }

    bool isTicketReservationPage = false;
    const QJsonArray trains = trainInfoFromDomSnapshot(result, &isTicketReservationPage);
    if (isTicketReservationPage) {
        m_trainInfoSessionId = request.sessionId;
        updateTrainInfoTable(trains);
    } else if (request.sessionId == m_trainInfoSessionId) {
        clearTrainInfoTable();
    }

    const QJsonObject redactedSnapshot = redactDomSnapshot(result);

    const QJsonObject document {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("captureId"), request.captureId},
        {QStringLiteral("capturedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("domSnapshot"), redactedSnapshot}
    };
    const QString filePath = QDir(request.captureDirectory).filePath(QStringLiteral("dom-snapshot.json"));
    if (!writeJsonFile(filePath, document)) {
        showStatus(tr("DOM 스냅샷을 저장하지 못했습니다."), true);
        return;
    }

    const RecorderSession session = m_recorderSessions.value(request.sessionId);
    appendSnapshotManifest(QFileInfo(request.captureDirectory).dir().absolutePath(),
                           {{QStringLiteral("schemaVersion"), 1},
                            {QStringLiteral("captureId"), request.captureId},
                            {QStringLiteral("capturedAt"), document.value(QStringLiteral("capturedAt"))},
                            {QStringLiteral("url"), session.url},
                            {QStringLiteral("title"), session.title},
                            {QStringLiteral("domSnapshotFile"),
                             QDir(request.captureId).filePath(QStringLiteral("dom-snapshot.json"))}});
    showStatus(tr("DOM 스냅샷을 저장했습니다: %1").arg(session.title));
}

bool MainWindow::writeJsonFile(const QString &filePath, const QJsonObject &document) const
{
    if (!QDir().mkpath(QFileInfo(filePath).absolutePath())) {
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(document).toJson(QJsonDocument::Indented)) < 0) {
        return false;
    }
    return file.commit();
}

void MainWindow::appendSnapshotManifest(const QString &directory, const QJsonObject &entry) const
{
    QFile file(QDir(directory).filePath(QStringLiteral("manifest.jsonl")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
    file.write("\n");
}

QJsonObject MainWindow::redactDomSnapshot(QJsonObject snapshot)
{
    QJsonArray strings = snapshot.value(QStringLiteral("strings")).toArray();
    const auto redactStringAt = [&strings](int index) {
        if (index >= 0 && index < strings.size()) {
            strings[index] = QStringLiteral("[REDACTED]");
        }
    };
    const auto stringAt = [&strings](int index) {
        return index >= 0 && index < strings.size() ? strings.at(index).toString() : QString();
    };

    QJsonArray documents = snapshot.value(QStringLiteral("documents")).toArray();
    for (int documentIndex = 0; documentIndex < documents.size(); ++documentIndex) {
        QJsonObject document = documents.at(documentIndex).toObject();
        QJsonObject nodes = document.value(QStringLiteral("nodes")).toObject();

        const auto removeInputValues = [&nodes, &redactStringAt](const QString &field) {
            if (!nodes.contains(field)) {
                return;
            }
            const QJsonArray values = nodes.value(field).toObject()
                                          .value(QStringLiteral("value"))
                                          .toArray();
            for (const QJsonValue &value : values) {
                redactStringAt(value.toInt(-1));
            }
            nodes.remove(field);
        };
        removeInputValues(QStringLiteral("inputValue"));
        removeInputValues(QStringLiteral("textValue"));

        const QJsonArray nodeNames = nodes.value(QStringLiteral("nodeName")).toArray();
        QJsonArray attributes = nodes.value(QStringLiteral("attributes")).toArray();
        for (int nodeIndex = 0; nodeIndex < attributes.size(); ++nodeIndex) {
            if (nodeIndex >= nodeNames.size()) {
                continue;
            }
            const int nodeNameIndex = nodeNames.at(nodeIndex).toInt(-1);
            const QString nodeName = stringAt(nodeNameIndex).toLower();
            if (nodeName != QStringLiteral("input")) {
                continue;
            }

            QJsonArray attributeIndexes = attributes.at(nodeIndex).toArray();
            for (int attributeIndex = 0;
                 attributeIndex + 1 < attributeIndexes.size();
                 attributeIndex += 2) {
                const QString attributeName = stringAt(attributeIndexes.at(attributeIndex).toInt(-1))
                                                  .toLower();
                if (attributeName == QStringLiteral("value")) {
                    redactStringAt(attributeIndexes.at(attributeIndex + 1).toInt(-1));
                }
            }
        }

        document.insert(QStringLiteral("nodes"), nodes);
        documents[documentIndex] = document;
    }

    snapshot.insert(QStringLiteral("strings"), strings);
    snapshot.insert(QStringLiteral("documents"), documents);
    return snapshot;
}

QString MainWindow::defaultSnapshotDirectory()
{
    QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDirectory.isEmpty()) {
        baseDirectory = QDir::tempPath();
    }
    return QDir(baseDirectory).filePath(QStringLiteral("page-captures"));
}

bool MainWindow::isRecordablePageUrl(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme != QStringLiteral("devtools")
           && scheme != QStringLiteral("chrome")
           && scheme != QStringLiteral("edge");
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
