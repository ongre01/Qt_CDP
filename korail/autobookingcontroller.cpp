#include "autobookingcontroller.h"

#include "../cdp/cdpclient.h"
#include "../recorder/pagerecorder.h"

#include <QJsonDocument>
#include <QTimer>

AutoBookingController::AutoBookingController(CdpClient *cdpClient, PageRecorder *pageRecorder,
                                             QObject *parent)
    : QObject(parent)
    , m_cdpClient(cdpClient)
    , m_pageRecorder(pageRecorder)
    , m_refreshTimer(new QTimer(this))
{
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &AutoBookingController::refreshSelectedTrainPage);
    connect(m_cdpClient, &CdpClient::commandResult, this, &AutoBookingController::onCommandResult);
    connect(m_cdpClient, &CdpClient::commandError, this, &AutoBookingController::onCommandError);
    connect(m_pageRecorder, &PageRecorder::monitoringStarted, this, &AutoBookingController::update);
}

void AutoBookingController::start()
{
    if (!m_running) {
        m_running = true;
        emit runningChanged(true);
    }
    update();
}

void AutoBookingController::stop()
{
    const bool wasRunning = m_running;
    m_running = false;
    m_refreshTimer->stop();
    resetBookingState();
    if (wasRunning) {
        emit runningChanged(false);
    }
}

bool AutoBookingController::isRunning() const
{
    return m_running;
}

void AutoBookingController::setRefreshIntervalSeconds(int seconds)
{
    m_refreshTimer->setInterval(seconds * 1000);
    if (m_running && m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
}

void AutoBookingController::setAutoBookWhenAvailable(bool enabled)
{
    m_autoBookWhenAvailable = enabled;
    if (enabled) {
        update();
    }
}

void AutoBookingController::setSelectedTrains(const QList<TrainInfo> &trains)
{
    m_selectedTrains = trains;
    update();
}

void AutoBookingController::setTrainInfoSession(const QString &sessionId)
{
    m_sessionId = sessionId;
    update();
}

void AutoBookingController::setTrainInfoContext(const QString &sessionId,
                                                const QList<TrainInfo> &trains)
{
    m_sessionId = sessionId;
    m_selectedTrains = trains;
    update();
}

void AutoBookingController::update()
{
    if (!m_running) {
        m_refreshTimer->stop();
        return;
    }
    if (m_bookingInProgress) {
        m_refreshTimer->stop();
        return;
    }
    const TrainInfo *train = reservableSelectedTrain();
    if (m_selectedTrains.isEmpty()) {
        m_refreshTimer->stop();
        emit statusChanged(tr("매크로가 준비되었습니다. 예매 가능 여부를 확인할 열차를 선택하세요."));
        return;
    }
    if (train) {
        if (m_autoBookWhenAvailable) {
            startAutoBooking(*train);
        } else {
            stop();
            emit statusChanged(tr("선택한 열차에서 예매 가능한 좌석을 찾았습니다."));
        }
        return;
    }
    if (!m_pageRecorder->isMonitoringActive() || m_sessionId.isEmpty()
        || !m_pageRecorder->hasSession(m_sessionId)) {
        return;
    }
    if (!m_refreshTimer->isActive()) {
        emit statusChanged(tr("선택한 열차의 예매 가능 여부를 확인 중입니다. %1초 후 페이지를 새로고침합니다.")
                               .arg(m_refreshTimer->interval() / 1000));
        m_refreshTimer->start();
    }
}

void AutoBookingController::refreshSelectedTrainPage()
{
    if (!m_running || m_selectedTrains.isEmpty() || m_bookingInProgress) {
        return;
    }
    if (reservableSelectedTrain()) {
        update();
        return;
    }
    if (!m_pageRecorder->isMonitoringActive() || m_sessionId.isEmpty()
        || !m_pageRecorder->hasSession(m_sessionId)) {
        return;
    }
    if (!m_pageRecorder->reloadPage(m_sessionId)) {
        stop();
        emit statusChanged(tr("선택한 열차 정보를 새로고침하지 못했습니다."));
        return;
    }
    emit statusChanged(tr("선택한 열차의 예매 가능 여부를 다시 확인하기 위해 페이지를 새로고침했습니다."));
}

const TrainInfo *AutoBookingController::reservableSelectedTrain() const
{
    for (const TrainInfo &train : m_selectedTrains) {
        if (train.generalReservable || train.specialReservable) {
            return &train;
        }
    }
    return nullptr;
}

void AutoBookingController::startAutoBooking(const TrainInfo &train)
{
    if (!m_running || m_bookingInProgress || !m_pageRecorder->isMonitoringActive()
        || m_sessionId.isEmpty() || !m_pageRecorder->hasSession(m_sessionId)) {
        return;
    }
    if (train.trainNo.isEmpty() || train.departureTime.isEmpty() || train.arrivalTime.isEmpty()) {
        stop();
        emit bookingFailed(tr("자동 예매에 필요한 열차 번호 또는 운행 시각을 확인하지 못했습니다."));
        emit statusChanged(tr("자동 예매에 필요한 열차 번호 또는 운행 시각을 확인하지 못했습니다."));
        return;
    }
    const QJsonObject trainJson {{QStringLiteral("trainType"), train.trainType},
                                 {QStringLiteral("trainNumber"), train.trainNo},
                                 {QStringLiteral("departure"), train.departure},
                                 {QStringLiteral("departureTime"), train.departureTime},
                                 {QStringLiteral("arrival"), train.arrival},
                                 {QStringLiteral("arrivalTime"), train.arrivalTime},
                                 {QStringLiteral("generalReservable"), train.generalReservable},
                                 {QStringLiteral("specialReservable"), train.specialReservable}};
    const QString expression = QStringLiteral(R"JS(
(() => {
    const train = %1;
    const clean = (value) => String(value || '').replace(/\s+/g, ' ').trim();
    const rowMatches = (row) => {
        const text = clean(row.innerText || row.textContent);
        return [train.trainType, train.trainNumber, train.departure, train.departureTime,
                train.arrival, train.arrivalTime]
            .filter(Boolean)
            .every((part) => text.includes(clean(part)));
    };
    const isReservable = (text) => {
        const value = clean(text);
        return (/예매|예약|\b\d{1,3}(?:,\d{3})*\s*원/.test(value))
            && !/매진|없음|불가|대기/.test(value);
    };
    const isUsable = (element) => {
        if (!element || element.disabled || element.getAttribute('aria-disabled') === 'true'
            || /disabled|disable|soldout|sold-out/i.test(element.className || '')) {
            return false;
        }
        const style = window.getComputedStyle(element);
        const rect = element.getBoundingClientRect();
        return style.display !== 'none' && style.visibility !== 'hidden'
            && rect.width > 0 && rect.height > 0;
    };
    const actionSelector = 'a, button, input[type="button"], input[type="submit"], [role="button"]';
    const actionText = (element) => clean([
        element.innerText, element.value, element.textContent,
        element.getAttribute('aria-label'), element.getAttribute('title')
    ].filter(Boolean).join(' '));
    const isReservationControl = (element) => isUsable(element) && isReservable(actionText(element));
    const rows = Array.from(document.querySelectorAll(
        'li.tckList, tr, [data-train-no], [data-trainno], [data-train-number]'
    )).map((element) => element.closest('li.tckList, tr') || element);
    const row = rows.find(rowMatches);
    if (!row) {
        return { found: false, message: '선택한 열차 행을 찾지 못했습니다.' };
    }
    const seatTypes = [
        { name: '일반실', selector: '.gen, .general, .normal, [class*="gen"], [class*="general"]', available: train.generalReservable },
        { name: '특실', selector: '.spe, .special, [class*="spe"], [class*="special"]', available: train.specialReservable }
    ];
    const rowControls = Array.from(row.querySelectorAll(actionSelector)).filter(isReservationControl);
    const pointFor = (control, seatType) => {
        control.scrollIntoView({ block: 'center', inline: 'center' });
        const rect = control.getBoundingClientRect();
        if (rect.width <= 0 || rect.height <= 0) {
            return null;
        }
        return { found: true, seatType, x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
    };
    for (const seat of seatTypes) {
        if (!seat.available) continue;
        for (const container of Array.from(row.querySelectorAll(seat.selector))) {
            if (!isReservable(container.innerText || container.textContent)) continue;
            const controls = (container.matches(actionSelector) ? [container] : [])
                .concat(Array.from(container.querySelectorAll(actionSelector)));
            const control = controls.find(isReservationControl);
            const point = control ? pointFor(control, seat.name) : null;
            if (point) return point;
            const containerPoint = pointFor(container, seat.name);
            if (containerPoint) return containerPoint;
        }
        const seatKeyword = seat.name === '일반실' ? /일반|general|normal|gen/i : /특실|special|spe/i;
        const control = rowControls.find((candidate) => {
            let current = candidate;
            for (let depth = 0; current && current !== row && depth < 4; ++depth, current = current.parentElement) {
                const context = clean([current.className, current.id, current.getAttribute('data-seat-class'),
                    current.getAttribute('aria-label'), current.innerText].filter(Boolean).join(' '));
                if (seatKeyword.test(context)) return true;
            }
            return false;
        });
        const point = control ? pointFor(control, seat.name) : null;
        if (point) return point;
    }
    if (rowControls.length === 1) return pointFor(rowControls[0], '예매 가능 좌석');
    return { found: false, message: '예매 가능한 좌석의 예매 버튼을 찾지 못했습니다.' };
})()
)JS").arg(QString::fromUtf8(QJsonDocument(trainJson).toJson(QJsonDocument::Compact)));

    resetBookingState();
    m_bookingInProgress = true;
    m_refreshTimer->stop();
    const int commandId = sendCommand(QStringLiteral("Runtime.evaluate"),
                                      {{QStringLiteral("expression"), expression}, {QStringLiteral("returnByValue"), true},
                                       {QStringLiteral("awaitPromise"), true}, {QStringLiteral("userGesture"), true}},
                                      m_sessionId);
    if (commandId == 0) {
        failBooking(tr("자동 예매 명령을 보낼 수 없습니다."));
        return;
    }
    m_requests.insert(commandId, RequestType::SelectSeat);
    emit bookingStarted();
    emit statusChanged(tr("예매 가능한 좌석을 찾아 자동 예매를 시도하는 중입니다..."));
}

void AutoBookingController::continueWithConfirmation(const QString &sessionId)
{
    if (!m_running || !m_bookingInProgress || sessionId.isEmpty()
        || !m_pageRecorder->isMonitoringActive() || !m_pageRecorder->hasSession(sessionId)) return;
    const QString expression = QStringLiteral(R"JS(
(() => {
    const clean = (value) => String(value || '').replace(/\s+/g, ' ').trim();
    const textFor = (element) => clean([element.innerText, element.value, element.textContent,
        element.getAttribute('aria-label'), element.getAttribute('title')].filter(Boolean).join(' '));
    const isUsable = (element) => {
        if (!element || element.disabled || element.getAttribute('aria-disabled') === 'true'
            || /disabled|disable/i.test(element.className || '')) return false;
        const style = window.getComputedStyle(element); const rect = element.getBoundingClientRect();
        return style.display !== 'none' && style.visibility !== 'hidden' && rect.width > 0 && rect.height > 0;
    };
    const isBookingLabel = (element) => /(^|\s)예매(?=\s|$)/.test(textFor(element)) && !/예약대기/.test(textFor(element));
    const bottomReservationButtons = Array.from(document.querySelectorAll(
        '.ticket_reserv_wrap button.reservbtn:not([disabled]), ' + '.ticket_reserv_wrap button.btn_bn-blue02:not([disabled])'
    )).filter((element) => isUsable(element) && isBookingLabel(element));
    const controls = bottomReservationButtons.length > 0 ? bottomReservationButtons
        : Array.from(document.querySelectorAll('body *')).filter((element) => isUsable(element) && isBookingLabel(element));
    if (controls.length === 0) return { found: false };
    controls.sort((left, right) => { const leftRect = left.getBoundingClientRect(); const rightRect = right.getBoundingClientRect();
        if (rightRect.top !== leftRect.top) return rightRect.top - leftRect.top;
        return (rightRect.width * rightRect.height) - (leftRect.width * leftRect.height); });
    controls[0].scrollIntoView({ block: 'center', inline: 'center' }); const rect = controls[0].getBoundingClientRect();
    return { found: true, x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
})()
)JS");
    const int commandId = sendCommand(QStringLiteral("Runtime.evaluate"),
                                      {{QStringLiteral("expression"), expression}, {QStringLiteral("returnByValue"), true},
                                       {QStringLiteral("awaitPromise"), true}, {QStringLiteral("userGesture"), true}}, sessionId);
    if (commandId == 0) { failBooking(tr("하단 예매 버튼을 확인할 수 없습니다.")); return; }
    m_requests.insert(commandId, RequestType::ConfirmBooking);
}

void AutoBookingController::continueWithInformationalDialogs(const QString &sessionId)
{
    if (!m_running || !m_bookingInProgress || sessionId.isEmpty()
        || !m_pageRecorder->isMonitoringActive() || !m_pageRecorder->hasSession(sessionId)) return;
    const QString expression = QStringLiteral(R"JS(
(() => {
    const clean = (value) => String(value || '').replace(/\s+/g, ' ').trim();
    const isVisible = (element) => { if (!element) return false; const style = window.getComputedStyle(element);
        const rect = element.getBoundingClientRect(); return style.display !== 'none' && style.visibility !== 'hidden' && rect.width > 0 && rect.height > 0; };
    const labelFor = (element) => clean(element.innerText || element.value || element.textContent || element.getAttribute('aria-label') || element.getAttribute('title'));
    const isUsable = (element) => isVisible(element) && !element.disabled && element.getAttribute('aria-disabled') !== 'true';
    const dialogs = [document.getElementById('layerPopup'), ...document.querySelectorAll('[role="dialog"], .layerPopup, .layer_wrap, .modal, .popup')]
        .filter((element, index, elements) => element && elements.indexOf(element) === index && isVisible(element));
    const popup = dialogs.find((element) => /이용안내/.test(clean(element.innerText || element.textContent))) || dialogs[0];
    if (!popup) return { found: false };
    const controls = Array.from(popup.querySelectorAll('button, a, input[type="button"], input[type="submit"], [role="button"]')).filter(isUsable);
    if (controls.some((element) => /^(취소|아니오)$/.test(labelFor(element)))) return { found: false };
    const control = controls.find((element) => element.matches('button.btn_pop-close')) || controls.find((element) => /^(확인|닫기|알겠습니다|예)$/.test(labelFor(element)));
    if (!control) return { found: false };
    if (%1) { const eventOptions = { bubbles: true, cancelable: true, view: window }; control.focus({ preventScroll: true });
        try { control.dispatchEvent(new PointerEvent('pointerdown', eventOptions)); } catch (_) {} control.dispatchEvent(new MouseEvent('mousedown', eventOptions));
        control.dispatchEvent(new MouseEvent('mouseup', eventOptions)); control.click(); return { found: true, clicked: true }; }
    control.scrollIntoView({ block: 'center', inline: 'center' }); const rect = control.getBoundingClientRect();
    return { found: rect.width > 0 && rect.height > 0, x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
})()
)JS").arg(m_dialogClicks > 0 && !m_dialogDomFallbackUsed ? QStringLiteral("true") : QStringLiteral("false"));
    const int commandId = sendCommand(QStringLiteral("Runtime.evaluate"),
                                      {{QStringLiteral("expression"), expression}, {QStringLiteral("returnByValue"), true},
                                       {QStringLiteral("awaitPromise"), true}, {QStringLiteral("userGesture"), true}}, sessionId);
    if (commandId == 0) { failBooking(tr("안내 메시지를 확인할 수 없습니다.")); return; }
    m_requests.insert(commandId, RequestType::DismissDialog);
}

void AutoBookingController::onCommandResult(int id, const QJsonObject &result)
{
    if (!m_requests.contains(id)) return;
    const RequestType request = m_requests.take(id);
    if (!result.value(QStringLiteral("exceptionDetails")).toObject().isEmpty()) {
        failBooking(tr("자동 예매 명령을 실행하지 못했습니다."));
        return;
    }
    const QJsonObject value = result.value(QStringLiteral("result")).toObject().value(QStringLiteral("value")).toObject();
    const auto dispatchMouse = [this](const QString &type, const QString &sessionId, RequestType next) {
        const int commandId = sendCommand(QStringLiteral("Input.dispatchMouseEvent"),
                                          {{QStringLiteral("type"), type}, {QStringLiteral("x"), m_clickX},
                                           {QStringLiteral("y"), m_clickY}, {QStringLiteral("button"), QStringLiteral("left")},
                                           {QStringLiteral("clickCount"), 1}}, sessionId);
        if (commandId == 0) return false;
        m_requests.insert(commandId, next);
        return true;
    };
    switch (request) {
    case RequestType::SelectSeat:
        if (!value.value(QStringLiteral("found")).toBool() || !value.value(QStringLiteral("x")).isDouble() || !value.value(QStringLiteral("y")).isDouble()) {
            failBooking(tr("자동 예매를 시작하지 못했습니다: %1").arg(value.value(QStringLiteral("message")).toString(tr("예매 버튼을 찾지 못했습니다.")))); return;
        }
        m_seatType = value.value(QStringLiteral("seatType")).toString(tr("선택한 좌석")); m_clickX = value.value(QStringLiteral("x")).toDouble(); m_clickY = value.value(QStringLiteral("y")).toDouble();
        if (!dispatchMouse(QStringLiteral("mousePressed"), m_sessionId, RequestType::SelectSeatMousePressed)) failBooking(tr("자동 예매 버튼에 마우스 입력을 보낼 수 없습니다."));
        return;
    case RequestType::SelectSeatMousePressed:
        if (!dispatchMouse(QStringLiteral("mouseReleased"), m_sessionId, RequestType::SelectSeatMouseReleased)) failBooking(tr("자동 예매 버튼에 마우스 입력을 완료하지 못했습니다."));
        return;
    case RequestType::SelectSeatMouseReleased:
        m_confirmationAttempts = m_dialogAttempts = m_dialogClicks = 0; m_dialogDomFallbackUsed = false;
        emit statusChanged(tr("%1 좌석을 선택했습니다. 하단 예매 버튼을 누르는 중입니다...").arg(m_seatType.isEmpty() ? tr("선택한") : m_seatType));
        QTimer::singleShot(250, this, [this, sessionId = m_sessionId]() { continueWithConfirmation(sessionId); }); return;
    case RequestType::ConfirmBooking:
        if (!value.value(QStringLiteral("found")).toBool() || !value.value(QStringLiteral("x")).isDouble() || !value.value(QStringLiteral("y")).isDouble()) {
            if (++m_confirmationAttempts < 10) { QTimer::singleShot(250, this, [this, sessionId = m_sessionId]() { continueWithConfirmation(sessionId); }); return; }
            failBooking(tr("좌석은 선택됐지만 하단 예매 버튼을 찾지 못했습니다.")); return;
        }
        m_clickX = value.value(QStringLiteral("x")).toDouble(); m_clickY = value.value(QStringLiteral("y")).toDouble();
        if (!dispatchMouse(QStringLiteral("mousePressed"), m_sessionId, RequestType::ConfirmMousePressed)) failBooking(tr("하단 예매 버튼에 마우스 입력을 보낼 수 없습니다."));
        return;
    case RequestType::ConfirmMousePressed:
        if (!dispatchMouse(QStringLiteral("mouseReleased"), m_sessionId, RequestType::ConfirmMouseReleased)) failBooking(tr("하단 예매 버튼에 마우스 입력을 완료하지 못했습니다."));
        return;
    case RequestType::ConfirmMouseReleased:
        m_dialogAttempts = m_dialogClicks = 0; m_dialogDomFallbackUsed = false;
        emit statusChanged(tr("하단 예매 버튼을 눌렀습니다. 안내 메시지를 확인하는 중입니다..."));
        QTimer::singleShot(150, this, [this, sessionId = m_sessionId]() { continueWithInformationalDialogs(sessionId); }); return;
    case RequestType::DismissDialog:
        if (value.value(QStringLiteral("clicked")).toBool()) { ++m_dialogClicks; m_dialogAttempts = 0; m_dialogDomFallbackUsed = true;
            emit statusChanged(tr("안내 메시지의 확인 버튼을 다시 눌렀습니다. 닫힘을 확인하는 중입니다..."));
            QTimer::singleShot(250, this, [this, sessionId = m_sessionId]() { continueWithInformationalDialogs(sessionId); }); return; }
        if (!value.value(QStringLiteral("found")).toBool() || !value.value(QStringLiteral("x")).isDouble() || !value.value(QStringLiteral("y")).isDouble()) {
            if (++m_dialogAttempts < 12) { QTimer::singleShot(250, this, [this, sessionId = m_sessionId]() { continueWithInformationalDialogs(sessionId); }); return; }
            if (m_dialogClicks > 0) {
                finishBookingSuccessfully(tr("안내 메시지를 확인하고 예매 화면으로 전환했습니다."));
            } else {
                resetBookingState(); stop();
                emit statusChanged(tr("하단 예매 버튼을 눌렀습니다. 예매 화면으로 전환되는지 확인하세요."));
            }
            return;
        }
        m_clickX = value.value(QStringLiteral("x")).toDouble(); m_clickY = value.value(QStringLiteral("y")).toDouble();
        if (!dispatchMouse(QStringLiteral("mousePressed"), m_sessionId, RequestType::DismissDialogMousePressed)) failBooking(tr("안내 메시지의 확인 버튼에 마우스 입력을 보낼 수 없습니다."));
        return;
    case RequestType::DismissDialogMousePressed:
        if (!dispatchMouse(QStringLiteral("mouseReleased"), m_sessionId, RequestType::DismissDialogMouseReleased)) failBooking(tr("안내 메시지의 확인 버튼에 마우스 입력을 완료하지 못했습니다."));
        return;
    case RequestType::DismissDialogMouseReleased:
        ++m_dialogClicks; m_dialogAttempts = 0;
        if (m_dialogClicks >= 3) { finishBookingSuccessfully(tr("안내 메시지를 확인하고 예매를 진행했습니다.")); return; }
        emit statusChanged(tr("안내 메시지의 확인 버튼을 눌렀습니다. 예매 화면으로 이동하는 중입니다..."));
        QTimer::singleShot(200, this, [this, sessionId = m_sessionId]() { continueWithInformationalDialogs(sessionId); }); return;
    }
}

void AutoBookingController::onCommandError(int id, const QString &message)
{
    if (m_requests.contains(id)) { m_requests.remove(id); failBooking(tr("자동 예매 명령을 실행하지 못했습니다: %1").arg(message)); }
}

void AutoBookingController::finishBookingSuccessfully(const QString &message)
{
    resetBookingState();
    stop();
    emit bookingSucceeded();
    emit statusChanged(message);
}

void AutoBookingController::resetBookingState()
{
    m_bookingInProgress = false; m_seatType.clear(); m_clickX = m_clickY = 0.0; m_confirmationAttempts = 0;
    m_dialogAttempts = 0; m_dialogClicks = 0; m_dialogDomFallbackUsed = false; m_requests.clear();
}

void AutoBookingController::failBooking(const QString &message)
{
    resetBookingState(); stop(); emit bookingFailed(message); emit statusChanged(message);
}

int AutoBookingController::sendCommand(const QString &method, const QJsonObject &parameters,
                                       const QString &sessionId)
{
    return m_cdpClient->sendCommand(method, parameters, sessionId);
}
