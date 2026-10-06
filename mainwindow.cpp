#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "cdp/cdpclient.h"
#include "korail/autobookingcontroller.h"
#include "korail/korailauthcontroller.h"
#include "korail/traininfoparser.h"
#include "recorder/pagerecorder.h"
#include "recorder/snapshotstorage.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QHeaderView>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTableWidgetItem>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_startupCdpClient(new CdpClient(this))
    , m_authCdpClient(new CdpClient(this))
    , m_recorderCdpClient(new CdpClient(this))
    , m_snapshotStorage(new SnapshotStorage)
    , m_authController(new KorailAuthController(m_authCdpClient, this))
    , m_pageRecorder(new PageRecorder(m_recorderCdpClient, m_snapshotStorage, this))
    , m_autoBookingController(new AutoBookingController(m_recorderCdpClient, m_pageRecorder, this))
    , m_notificationNetworkManager(new QNetworkAccessManager(this))
{
    ui->setupUi(this);
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Qt_CDP"), QStringLiteral("Qt_CDP"));
    settings.beginGroup(QStringLiteral("KorailLogin"));
    ui->memberNumberEdit->setText(settings.value(QStringLiteral("memberNumber")).toString());
    ui->korailPasswordEdit->setText(settings.value(QStringLiteral("password")).toString());
    settings.endGroup();
    ui->snapshotDirectoryEdit->setText(SnapshotStorage::defaultDirectory());
    ui->trainInfoTableWidget->setColumnCount(10);
    ui->trainInfoTableWidget->setHorizontalHeaderLabels(
        {tr("선택"), tr("열차"), tr("번호"), tr("출발역"), tr("출발 시각"),
         tr("도착역"), tr("도착 시각"), tr("소요 시간"), tr("일반실"), tr("특실")});
    ui->trainInfoTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->trainInfoTableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->trainInfoTableWidget->verticalHeader()->setVisible(false);
    ui->trainInfoTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->trainInfoTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (열차 조회 페이지 대기 중)"));
    ui->trainInfoGroupBox->setVisible(true);

    connect(ui->startChromeButton, &QPushButton::clicked, this, &MainWindow::startChromeForCdp);
    connect(ui->korailAutoLoginButton, &QPushButton::clicked, this, &MainWindow::startKorailAutoLogin);
    connect(ui->pageRecordingButton, &QPushButton::clicked, this, &MainWindow::togglePageRecording);
    connect(ui->openSnapshotDirectoryButton, &QPushButton::clicked, this, &MainWindow::openSnapshotDirectory);
    connect(ui->trainInfoTableWidget, &QTableWidget::itemChanged, this, &MainWindow::onTrainInfoItemChanged);
    connect(ui->startTrainRefreshMacroButton, &QPushButton::clicked, this, &MainWindow::startTrainRefreshMacro);
    connect(ui->stopTrainRefreshMacroButton, &QPushButton::clicked, this, [this]() {
        stopTrainRefreshMacro(tr("열차 예매 확인 매크로를 중지했습니다."));
    });
    connect(ui->macroRefreshIntervalSpinBox, &QSpinBox::valueChanged, this,
            [this](int seconds) { m_autoBookingController->setRefreshIntervalSeconds(seconds); });
    connect(ui->autoBookWhenAvailableCheckBox, &QCheckBox::toggled, this,
            [this](bool enabled) { m_autoBookingController->setAutoBookWhenAvailable(enabled); });

    m_autoBookingController->setRefreshIntervalSeconds(ui->macroRefreshIntervalSpinBox->value());
    m_autoBookingController->setAutoBookWhenAvailable(ui->autoBookWhenAvailableCheckBox->isChecked());

    connect(m_startupCdpClient, &CdpClient::chromeStarted, this, [this](qint64 processId) {
        setBusy(true);
        showStatus(tr("CDP용 Chrome(PID: %1)을 시작했습니다. 디버거 연결을 기다리는 중입니다...")
                       .arg(processId));
    });
    connect(m_startupCdpClient, &CdpClient::chromeEndpointReady, this, [this](const QUrl &) {
        setBusy(false);
        showStatus(tr("CDP용 Chrome이 준비되었습니다. Qt에서 CDP 제어를 시작할 수 있습니다."));
        startTrainInfoMonitoring();
    });
    connect(m_startupCdpClient, &CdpClient::errorOccurred, this, [this](const QString &message) {
        setBusy(false);
        showStatus(message, true);
    });

    connect(m_authController, &KorailAuthController::loginStarted, this, [this]() { setBusy(true); });
    connect(m_authController, &KorailAuthController::statusChanged, this,
            [this](const QString &message) { showStatus(message); });
    connect(m_authController, &KorailAuthController::loginSucceeded, this, [this]() {
        ui->korailPasswordEdit->clear();
        setBusy(false);
    });
    connect(m_authController, &KorailAuthController::loginFailed, this, [this](const QString &message) {
        ui->korailPasswordEdit->clear();
        setBusy(false);
        showStatus(message, true);
    });

    connect(m_pageRecorder, &PageRecorder::pageRecordingChanged, this, &MainWindow::updatePageRecordingUi);
    connect(m_pageRecorder, &PageRecorder::snapshotCaptured, this, &MainWindow::onDomSnapshotCaptured);
    connect(m_pageRecorder, &PageRecorder::pageDetached, this, [this](const QString &sessionId) {
        if (sessionId == m_trainInfoSessionId) {
            clearTrainInfoTable();
        }
    });
    connect(m_pageRecorder, &PageRecorder::monitoringStopped, this, &MainWindow::clearTrainInfoTable);
    connect(m_pageRecorder, &PageRecorder::statusChanged, this,
            [this](const QString &message) { showStatus(message); });
    connect(m_pageRecorder, &PageRecorder::errorOccurred, this,
            [this](const QString &message) { showStatus(message, true); });

    connect(m_autoBookingController, &AutoBookingController::runningChanged, this, [this](bool running) {
        ui->startTrainRefreshMacroButton->setEnabled(!running);
        ui->stopTrainRefreshMacroButton->setEnabled(running);
    });
    connect(m_autoBookingController, &AutoBookingController::statusChanged, this,
            [this](const QString &message) { showStatus(message); });
    connect(m_autoBookingController, &AutoBookingController::bookingFailed, this,
            [this](const QString &message) { showStatus(message, true); });
    connect(m_autoBookingController, &AutoBookingController::bookingSucceeded, this, [this]() {
        QNetworkRequest request(QUrl(QStringLiteral("https://ntfy.sh/ktx")));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("text/plain; charset=utf-8"));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setRawHeader("Title", QStringLiteral("KTX 예매 완료").toUtf8());
        request.setRawHeader("Priority", "high");

        QNetworkReply *reply = m_notificationNetworkManager->post(
            request, QStringLiteral("KTX 예매가 완료되었습니다.").toUtf8());
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
                showStatus(tr("ntfy 예매 완료 알림을 전송하지 못했습니다: %1")
                               .arg(reply->errorString()), true);
            } else {
                showStatus(tr("ntfy 예매 완료 알림을 전송했습니다 (HTTP %1).")
                               .arg(statusCode));
            }
            reply->deleteLater();
        });
    });
}

MainWindow::~MainWindow()
{
    delete m_snapshotStorage;
    delete ui;
}

void MainWindow::startChromeForCdp()
{
    QString errorMessage;
    const QUrl versionUrl = CdpClient::debuggerVersionUrl(ui->debuggerEndpointEdit->text(), &errorMessage);
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    m_startupCdpClient->startChrome(ui->chromeExecutableEdit->text().trimmed(),
                                    ui->userDataDirEdit->text().trimmed(), versionUrl);
}

void MainWindow::startKorailAutoLogin()
{
    const QString memberNumber = ui->memberNumberEdit->text().trimmed();
    const QString password = ui->korailPasswordEdit->text();
    QString errorMessage;
    const QUrl versionUrl = CdpClient::debuggerVersionUrl(ui->debuggerEndpointEdit->text(), &errorMessage);
    if (memberNumber.isEmpty() || password.isEmpty()) {
        showStatus(tr("코레일 회원번호와 비밀번호를 모두 입력하세요."), true);
        return;
    }
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    if (!CdpClient::isLocalHost(versionUrl.host())) {
        showStatus(tr("자동 로그인은 로컬 CDP 주소에서만 실행할 수 있습니다."), true);
        return;
    }
    if (m_authController->isLoggingIn()) {
        return;
    }

    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Qt_CDP"), QStringLiteral("Qt_CDP"));
    settings.beginGroup(QStringLiteral("KorailLogin"));
    settings.setValue(QStringLiteral("memberNumber"), memberNumber);
    settings.setValue(QStringLiteral("password"), password);
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        showStatus(tr("코레일 로그인 정보를 INI 파일에 저장하지 못했습니다."), true);
        return;
    }

    startTrainInfoMonitoring();
    m_authController->login(memberNumber, password, versionUrl);
}

void MainWindow::togglePageRecording()
{
    if (m_pageRecorder->isPageRecordingActive()) {
        m_pageRecorder->stopPageRecording();
        showStatus(tr("방문 페이지 기록을 중지했습니다."));
        return;
    }
    QString errorMessage;
    const QUrl versionUrl = CdpClient::debuggerVersionUrl(ui->debuggerEndpointEdit->text(), &errorMessage);
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    if (!CdpClient::isLocalHost(versionUrl.host())) {
        showStatus(tr("열차 정보 수집은 로컬 CDP 주소에서만 지원합니다."), true);
        return;
    }
    ui->pageRecordingButton->setEnabled(false);
    if (!m_pageRecorder->startPageRecording(versionUrl, ui->snapshotDirectoryEdit->text().trimmed())) {
        ui->pageRecordingButton->setEnabled(true);
    }
}

void MainWindow::openSnapshotDirectory()
{
    const QString snapshotDirectory = ui->snapshotDirectoryEdit->text().trimmed();
    if (snapshotDirectory.isEmpty()) {
        showStatus(tr("스냅샷 저장 폴더를 입력하세요."), true);
        return;
    }
    if (!QDir().mkpath(snapshotDirectory)) {
        showStatus(tr("스냅샷 저장 폴더를 만들 수 없습니다."), true);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(snapshotDirectory).absolutePath()))) {
        showStatus(tr("스냅샷 저장 폴더를 열 수 없습니다."), true);
    }
}

void MainWindow::startTrainInfoMonitoring()
{
    if (m_pageRecorder->isMonitoringRequested() || m_pageRecorder->isMonitoringActive()) {
        return;
    }
    QString errorMessage;
    const QUrl versionUrl = CdpClient::debuggerVersionUrl(ui->debuggerEndpointEdit->text(), &errorMessage);
    if (!versionUrl.isValid()) {
        showStatus(errorMessage, true);
        return;
    }
    if (!CdpClient::isLocalHost(versionUrl.host())) {
        showStatus(tr("열차 정보 수집은 로컬 CDP 주소에서만 지원합니다."), true);
        return;
    }
    m_pageRecorder->startMonitoring(versionUrl);
}

void MainWindow::startTrainRefreshMacro()
{
    startTrainInfoMonitoring();
    m_autoBookingController->start();
}

void MainWindow::stopTrainRefreshMacro(const QString &message)
{
    const bool wasRunning = m_autoBookingController->isRunning();
    m_autoBookingController->stop();
    if (wasRunning && !message.isEmpty()) {
        showStatus(message);
    }
}

void MainWindow::onDomSnapshotCaptured(const QJsonObject &snapshot, const QString &sessionId)
{
    bool isTicketReservationPage = false;
    const QList<TrainInfo> trains = TrainInfoParser::parse(snapshot, &isTicketReservationPage);
    if (isTicketReservationPage) {
        m_trainInfoSessionId = sessionId;
        updateTrainInfoTable(trains);
        m_autoBookingController->setTrainInfoContext(sessionId, selectedTrains());
    } else if (sessionId == m_trainInfoSessionId) {
        clearTrainInfoTable();
    }
}

void MainWindow::updateTrainInfoTable(const QList<TrainInfo> &trains)
{
    m_currentTrains = trains;
    m_updatingTrainInfoTable = true;
    ui->trainInfoTableWidget->setUpdatesEnabled(false);
    ui->trainInfoTableWidget->setRowCount(0);
    for (const TrainInfo &train : trains) {
        const int row = ui->trainInfoTableWidget->rowCount();
        ui->trainInfoTableWidget->insertRow(row);
        const QString selectionKey = TrainInfoParser::selectionKey(train);
        auto *selectionItem = new QTableWidgetItem;
        selectionItem->setFlags(selectionItem->flags() | Qt::ItemIsUserCheckable);
        selectionItem->setData(Qt::UserRole, selectionKey);
        selectionItem->setCheckState(m_selectedTrainKeys.contains(selectionKey) ? Qt::Checked : Qt::Unchecked);
        selectionItem->setToolTip(tr("이 열차 선택"));
        ui->trainInfoTableWidget->setItem(row, 0, selectionItem);
        const QStringList columns {train.trainType, train.trainNo, train.departure, train.departureTime,
                                   train.arrival, train.arrivalTime, train.duration, train.generalSeat,
                                   train.specialSeat};
        for (int column = 0; column < columns.size(); ++column) {
            auto *item = new QTableWidgetItem(columns.at(column));
            item->setToolTip(columns.at(column));
            ui->trainInfoTableWidget->setItem(row, column + 1, item);
        }
    }
    ui->trainInfoTableWidget->setUpdatesEnabled(true);
    m_updatingTrainInfoTable = false;
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (%1건)").arg(trains.size()));
    ui->trainInfoGroupBox->setVisible(true);
}

void MainWindow::clearTrainInfoTable()
{
    stopTrainRefreshMacro();
    m_currentTrains.clear();
    m_selectedTrainKeys.clear();
    m_trainInfoSessionId.clear();
    m_autoBookingController->setTrainInfoContext({}, {});
    ui->trainInfoTableWidget->setRowCount(0);
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (열차 조회 페이지 대기 중)"));
    ui->trainInfoGroupBox->setVisible(true);
}

void MainWindow::onTrainInfoItemChanged(QTableWidgetItem *item)
{
    if (m_updatingTrainInfoTable || !item || item->column() != 0) {
        return;
    }
    const QString selectionKey = item->data(Qt::UserRole).toString();
    if (selectionKey.isEmpty()) {
        return;
    }
    if (item->checkState() == Qt::Checked) {
        m_selectedTrainKeys.insert(selectionKey);
    } else {
        m_selectedTrainKeys.remove(selectionKey);
    }
    m_autoBookingController->setSelectedTrains(selectedTrains());
}

QList<TrainInfo> MainWindow::selectedTrains() const
{
    QList<TrainInfo> selected;
    for (const TrainInfo &train : m_currentTrains) {
        if (m_selectedTrainKeys.contains(TrainInfoParser::selectionKey(train))) {
            selected.append(train);
        }
    }
    return selected;
}

void MainWindow::updatePageRecordingUi(bool active)
{
    ui->pageRecordingButton->setEnabled(true);
    ui->pageRecordingButton->setText(active ? tr("방문 페이지 기록 중지") : tr("방문 페이지 기록 시작"));
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
    ui->statusLabel->setStyleSheet(isError ? QStringLiteral("color: #b00020;")
                                      : QStringLiteral("color: #1b5e20;"));
}
