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
#include <QTabWidget>
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
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (열차 조회 페이지 대기 중)"));
    ui->trainInfoGroupBox->setVisible(true);

    connect(ui->startChromeButton, &QPushButton::clicked, this, &MainWindow::startChromeForCdp);
    connect(ui->korailAutoLoginButton, &QPushButton::clicked, this, &MainWindow::startKorailAutoLogin);
    connect(ui->pageRecordingButton, &QPushButton::clicked, this, &MainWindow::togglePageRecording);
    connect(ui->openSnapshotDirectoryButton, &QPushButton::clicked, this, &MainWindow::openSnapshotDirectory);
    connect(ui->startTrainRefreshMacroButton, &QPushButton::clicked, this, &MainWindow::startTrainRefreshMacro);
    connect(ui->stopTrainRefreshMacroButton, &QPushButton::clicked, this, [this]() {
        stopTrainRefreshMacro(tr("열차 예매 확인 매크로를 중지했습니다."));
    });
    connect(ui->macroRefreshIntervalSpinBox, &QSpinBox::valueChanged, this,
            [this](int seconds) {
                for (const TrainInfoTab &tab : m_trainInfoTabs) {
                    tab.controller->setRefreshIntervalSeconds(seconds);
                }
            });
    connect(ui->autoBookWhenAvailableCheckBox, &QCheckBox::toggled, this,
            [this](bool enabled) {
                for (const TrainInfoTab &tab : m_trainInfoTabs) {
                    tab.controller->setAutoBookWhenAvailable(enabled);
                }
            });

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
        setBusy(false);
    });
    connect(m_authController, &KorailAuthController::loginFailed, this, [this](const QString &message) {
        setBusy(false);
        showStatus(message, true);
    });

    connect(m_pageRecorder, &PageRecorder::pageRecordingChanged, this, &MainWindow::updatePageRecordingUi);
    connect(m_pageRecorder, &PageRecorder::snapshotCaptured, this, &MainWindow::onDomSnapshotCaptured);
    connect(m_pageRecorder, &PageRecorder::pageDetached, this, [this](const QString &sessionId) {
        removeTrainInfoTab(sessionId);
    });
    connect(m_pageRecorder, &PageRecorder::monitoringStopped, this, &MainWindow::clearTrainInfoTable);
    connect(m_pageRecorder, &PageRecorder::statusChanged, this,
            [this](const QString &message) { showStatus(message); });
    connect(m_pageRecorder, &PageRecorder::errorOccurred, this,
            [this](const QString &message) { showStatus(message, true); });

    updateTrainRefreshMacroUi();
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
    if (m_trainRefreshMacroRunning) {
        return;
    }

    m_trainRefreshMacroRunning = true;
    for (const TrainInfoTab &tab : m_trainInfoTabs) {
        tab.controller->start();
    }
    updateTrainRefreshMacroUi();

    if (m_trainInfoTabs.isEmpty()) {
        showStatus(tr("병렬 예매 매크로가 준비되었습니다. 코레일 열차 조회 탭을 하나 이상 여세요."));
    }
}

void MainWindow::stopTrainRefreshMacro(const QString &message)
{
    const bool wasRunning = m_trainRefreshMacroRunning;
    m_trainRefreshMacroRunning = false;
    for (const TrainInfoTab &tab : m_trainInfoTabs) {
        tab.controller->stop();
    }
    updateTrainRefreshMacroUi();
    if (wasRunning && !message.isEmpty()) {
        showStatus(message);
    }
}

void MainWindow::onDomSnapshotCaptured(const QJsonObject &snapshot, const QString &sessionId)
{
    bool isTicketReservationPage = false;
    const QList<TrainInfo> trains = TrainInfoParser::parse(snapshot, &isTicketReservationPage);
    if (isTicketReservationPage) {
        if (sessionId.isEmpty()) {
            return;
        }
        if (!m_trainInfoTabs.contains(sessionId)) {
            const QString label = tr("탭 %1").arg(m_nextTrainInfoTabNumber++);
            TrainInfoTab tab;
            tab.number = m_nextTrainInfoTabNumber - 1;
            tab.controller = createAutoBookingController(sessionId, label);
            tab.table = createTrainInfoTable(sessionId);
            ui->trainInfoTabWidget->addTab(tab.table, label);
            m_trainInfoTabs.insert(sessionId, tab);
        }

        TrainInfoTab &tab = m_trainInfoTabs[sessionId];
        if (trains.isEmpty() && m_trainRefreshMacroRunning && !tab.trains.isEmpty()) {
            return;
        }
        tab.trains = trains;
        tab.controller->setTrainInfoContext(sessionId, selectedTrains(sessionId));
        if (m_trainRefreshMacroRunning) {
            tab.controller->start();
        }
        updateTrainInfoTable();
    } else if (!m_trainRefreshMacroRunning) {
        removeTrainInfoTab(sessionId);
    }
}

void MainWindow::updateTrainInfoTable()
{
    m_updatingTrainInfoTable = true;
    int trainCount = 0;
    for (auto iterator = m_trainInfoTabs.cbegin(); iterator != m_trainInfoTabs.cend(); ++iterator) {
        const QString &sessionId = iterator.key();
        const TrainInfoTab &tab = iterator.value();
        QTableWidget *table = tab.table;
        table->setUpdatesEnabled(false);
        table->setRowCount(0);
        for (const TrainInfo &train : tab.trains) {
            const int row = table->rowCount();
            table->insertRow(row);
            const QString selectionKey = trainSelectionKey(sessionId, train);

            auto *selectionItem = new QTableWidgetItem;
            selectionItem->setFlags(selectionItem->flags() | Qt::ItemIsUserCheckable);
            selectionItem->setData(Qt::UserRole, selectionKey);
            selectionItem->setCheckState(m_selectedTrainKeys.contains(selectionKey) ? Qt::Checked : Qt::Unchecked);
            selectionItem->setToolTip(tr("이 열차 선택"));
            table->setItem(row, 0, selectionItem);

            const QStringList columns {train.trainType, train.trainNo, train.departure, train.departureTime,
                                       train.arrival, train.arrivalTime, train.duration, train.generalSeat,
                                       train.specialSeat};
            for (int column = 0; column < columns.size(); ++column) {
                auto *item = new QTableWidgetItem(columns.at(column));
                item->setToolTip(columns.at(column));
                table->setItem(row, column + 1, item);
            }
            ++trainCount;
        }
        table->setUpdatesEnabled(true);
    }
    m_updatingTrainInfoTable = false;
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (%1개 탭, %2건)")
                                        .arg(m_trainInfoTabs.size())
                                        .arg(trainCount));
    ui->trainInfoGroupBox->setVisible(true);
}

void MainWindow::clearTrainInfoTable()
{
    stopTrainRefreshMacro();
    for (const TrainInfoTab &tab : m_trainInfoTabs) {
        delete tab.controller;
        delete tab.table;
    }
    m_trainInfoTabs.clear();
    m_selectedTrainKeys.clear();
    m_nextTrainInfoTabNumber = 1;
    ui->trainInfoTabWidget->clear();
    ui->trainInfoGroupBox->setTitle(tr("열차 정보 (열차 조회 페이지 대기 중)"));
    ui->trainInfoGroupBox->setVisible(true);
}

void MainWindow::removeTrainInfoTab(const QString &sessionId)
{
    const auto iterator = m_trainInfoTabs.find(sessionId);
    if (iterator == m_trainInfoTabs.end()) {
        return;
    }

    delete iterator->controller;
    const int tabIndex = ui->trainInfoTabWidget->indexOf(iterator->table);
    if (tabIndex >= 0) {
        ui->trainInfoTabWidget->removeTab(tabIndex);
    }
    delete iterator->table;
    m_trainInfoTabs.erase(iterator);
    for (auto selected = m_selectedTrainKeys.begin(); selected != m_selectedTrainKeys.end();) {
        if (selected->startsWith(sessionId + QChar(0x1e))) {
            selected = m_selectedTrainKeys.erase(selected);
        } else {
            ++selected;
        }
    }
    updateTrainInfoTable();
}

void MainWindow::onTrainInfoItemChanged(const QString &sessionId, QTableWidgetItem *item)
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
    const auto iterator = m_trainInfoTabs.constFind(sessionId);
    if (iterator != m_trainInfoTabs.cend()) {
        iterator->controller->setSelectedTrains(selectedTrains(sessionId));
    }
}

QList<TrainInfo> MainWindow::selectedTrains(const QString &sessionId) const
{
    QList<TrainInfo> selected;
    const auto iterator = m_trainInfoTabs.constFind(sessionId);
    if (iterator == m_trainInfoTabs.cend()) {
        return selected;
    }
    for (const TrainInfo &train : iterator->trains) {
        if (m_selectedTrainKeys.contains(trainSelectionKey(sessionId, train))) {
            selected.append(train);
        }
    }
    return selected;
}

QString MainWindow::trainSelectionKey(const QString &sessionId, const TrainInfo &train) const
{
    return sessionId + QChar(0x1e) + TrainInfoParser::selectionKey(train);
}

QTableWidget *MainWindow::createTrainInfoTable(const QString &sessionId)
{
    auto *table = new QTableWidget(ui->trainInfoTabWidget);
    table->setColumnCount(10);
    table->setHorizontalHeaderLabels(
        {tr("선택"), tr("열차"), tr("번호"), tr("출발역"), tr("출발 시각"),
         tr("도착역"), tr("도착 시각"), tr("소요 시간"), tr("일반실"), tr("특실")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(table, &QTableWidget::itemChanged, this,
            [this, sessionId](QTableWidgetItem *item) {
                onTrainInfoItemChanged(sessionId, item);
            });
    return table;
}

AutoBookingController *MainWindow::createAutoBookingController(const QString &sessionId,
                                                                const QString &label)
{
    auto *controller = new AutoBookingController(m_recorderCdpClient, m_pageRecorder, this);
    controller->setRefreshIntervalSeconds(ui->macroRefreshIntervalSpinBox->value());
    controller->setAutoBookWhenAvailable(ui->autoBookWhenAvailableCheckBox->isChecked());
    controller->setTrainInfoSession(sessionId);

    connect(controller, &AutoBookingController::statusChanged, this,
            [this, label](const QString &message) {
                showStatus(tr("%1: %2").arg(label, message));
            });
    connect(controller, &AutoBookingController::bookingFailed, this,
            [this, label](const QString &message) {
                showStatus(tr("%1: %2").arg(label, message), true);
            });
    connect(controller, &AutoBookingController::bookingSucceeded, this, [this, label]() {
        stopTrainRefreshMacro();
        sendBookingNotification(label);
    });
    return controller;
}

void MainWindow::updateTrainRefreshMacroUi()
{
    ui->startTrainRefreshMacroButton->setEnabled(!m_trainRefreshMacroRunning);
    ui->stopTrainRefreshMacroButton->setEnabled(m_trainRefreshMacroRunning);
}

void MainWindow::sendBookingNotification(const QString &tab)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://ntfy.sh/ktx")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("text/plain; charset=utf-8"));
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("Title", QStringLiteral("KTX 예매 완료").toUtf8());
    request.setRawHeader("Priority", "high");

    QNetworkReply *reply = m_notificationNetworkManager->post(
        request, tr("%1에서 KTX 예매가 완료되었습니다.").arg(tab).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, tab]() {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
            showStatus(tr("%1: ntfy 예매 완료 알림을 전송하지 못했습니다: %2")
                           .arg(tab, reply->errorString()), true);
        } else {
            showStatus(tr("%1: ntfy 예매 완료 알림을 전송했습니다 (HTTP %2).")
                           .arg(tab)
                           .arg(statusCode));
        }
        reply->deleteLater();
    });
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
