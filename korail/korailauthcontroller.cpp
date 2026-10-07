#include "korailauthcontroller.h"

#include "../cdp/cdpclient.h"

#include <QTimer>

KorailAuthController::KorailAuthController(CdpClient *cdpClient, QObject *parent)
    : QObject(parent)
    , m_cdpClient(cdpClient)
{
    connect(m_cdpClient, &CdpClient::connected, this, &KorailAuthController::onConnected);
    connect(m_cdpClient, &CdpClient::disconnected, this, [this]() {
        if (m_inProgress) {
            finish(tr("Chrome CDP 연결이 끊어졌습니다."), true);
        }
    });
    connect(m_cdpClient, &CdpClient::commandResult, this, &KorailAuthController::onCommandResult);
    connect(m_cdpClient, &CdpClient::commandError, this, &KorailAuthController::onCommandError);
    connect(m_cdpClient, &CdpClient::errorOccurred, this, [this](const QString &message) {
        if (m_inProgress) {
            finish(message, true);
        }
    });
}

void KorailAuthController::login(const QString &id, const QString &password, const QUrl &versionUrl)
{
    if (m_inProgress) {
        return;
    }
    m_memberNumber = id;
    m_password = password;
    m_inProgress = true;
    m_step = LoginStep::Idle;
    m_pendingCommandId = 0;
    m_formCheckAttempts = 0;
    m_resultCheckAttempts = 0;
    m_transientErrorRetryCount = 0;
    m_dismissCheckAttempts = 0;
    m_sessionId.clear();
    m_loginButtonX = 0.0;
    m_loginButtonY = 0.0;
    m_dialogButtonX = 0.0;
    m_dialogButtonY = 0.0;
    emit loginStarted();
    emit statusChanged(tr("코레일 로그인용 Chrome 탭에 연결하는 중입니다..."));
    m_cdpClient->connectToChrome(versionUrl);
}

bool KorailAuthController::isLoggingIn() const
{
    return m_inProgress;
}

void KorailAuthController::onConnected()
{
    if (!m_inProgress) {
        m_cdpClient->close();
        return;
    }
    m_step = LoginStep::CreatingTarget;
    sendCommand(QStringLiteral("Target.createTarget"),
                {{QStringLiteral("url"), QStringLiteral("about:blank")}});
}

void KorailAuthController::onCommandResult(int id, const QJsonObject &result)
{
    if (!m_inProgress || id != m_pendingCommandId) {
        return;
    }
    if (result.contains(QStringLiteral("exceptionDetails"))) {
        finish(tr("코레일 로그인 페이지에서 자동화 스크립트를 실행하지 못했습니다."), true);
        return;
    }

    switch (m_step) {
    case LoginStep::CreatingTarget: {
        const QString targetId = result.value(QStringLiteral("targetId")).toString();
        if (targetId.isEmpty()) {
            finish(tr("코레일 로그인용 Chrome 탭을 만들지 못했습니다."), true);
            return;
        }
        m_step = LoginStep::AttachingTarget;
        sendCommand(QStringLiteral("Target.attachToTarget"),
                    {{QStringLiteral("targetId"), targetId}, {QStringLiteral("flatten"), true}});
        return;
    }
    case LoginStep::AttachingTarget:
        m_sessionId = result.value(QStringLiteral("sessionId")).toString();
        if (m_sessionId.isEmpty()) {
            finish(tr("코레일 로그인 탭에 연결하지 못했습니다."), true);
            return;
        }
        m_step = LoginStep::EnablingNetwork;
        sendCommand(QStringLiteral("Network.enable"));
        return;
    case LoginStep::EnablingNetwork:
        m_step = LoginStep::DisablingCache;
        sendCommand(QStringLiteral("Network.setCacheDisabled"),
                    {{QStringLiteral("cacheDisabled"), true}});
        return;
    case LoginStep::DisablingCache:
        m_step = LoginStep::EnablingPage;
        sendCommand(QStringLiteral("Page.enable"));
        return;
    case LoginStep::EnablingPage:
        m_step = LoginStep::Navigating;
        sendCommand(QStringLiteral("Page.navigate"),
                    {{QStringLiteral("url"), QStringLiteral("https://www.korail.com/ticket/login")}});
        return;
    case LoginStep::Navigating:
        m_formCheckAttempts = 0;
        QTimer::singleShot(200, this, &KorailAuthController::waitForLoginForm);
        return;
    case LoginStep::WaitingForLoginForm: {
        const bool formReady = result.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("value")).toBool();
        if (formReady) {
            submitLogin();
            return;
        }
        if (++m_formCheckAttempts >= 50) {
            finish(tr("코레일 로그인 페이지의 입력란을 찾지 못했습니다."), true);
            return;
        }
        QTimer::singleShot(200, this, &KorailAuthController::waitForLoginForm);
        return;
    }
    case LoginStep::FocusingMemberNumber:
        if (!result.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("value")).toBool()) {
            finish(tr("코레일 회원번호 입력란에 포커스를 둘 수 없습니다."), true);
            return;
        }
        m_step = LoginStep::EnteringMemberNumber;
        sendCommand(QStringLiteral("Input.insertText"),
                    {{QStringLiteral("text"), m_memberNumber}});
        return;
    case LoginStep::EnteringMemberNumber:
        focusPasswordField();
        return;
    case LoginStep::FocusingPassword:
        if (!result.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("value")).toBool()) {
            finish(tr("코레일 비밀번호 입력란에 포커스를 둘 수 없습니다."), true);
            return;
        }
        m_step = LoginStep::EnteringPassword;
        sendCommand(QStringLiteral("Input.insertText"),
                    {{QStringLiteral("text"), m_password}});
        return;
    case LoginStep::EnteringPassword:
        m_step = LoginStep::LocatingLoginButton;
        sendCommand(QStringLiteral("Runtime.evaluate"),
                    {{QStringLiteral("expression"),
                      QStringLiteral("(() => { const button = document.querySelector('#tab_memNum .btn_bn-depblue'); if (!button) return null; const rect = button.getBoundingClientRect(); return { x: rect.left + (rect.width / 2), y: rect.top + (rect.height / 2) }; })()")},
                     {QStringLiteral("returnByValue"), true}});
        return;
    case LoginStep::LocatingLoginButton: {
        const QJsonObject value = result.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("value")).toObject();
        if (!value.value(QStringLiteral("x")).isDouble()
            || !value.value(QStringLiteral("y")).isDouble()) {
            finish(tr("코레일 로그인 버튼의 위치를 확인하지 못했습니다."), true);
            return;
        }
        m_loginButtonX = value.value(QStringLiteral("x")).toDouble();
        m_loginButtonY = value.value(QStringLiteral("y")).toDouble();
        pressLoginButton();
        return;
    }
    case LoginStep::PressingLoginButton:
        m_step = LoginStep::ReleasingLoginButton;
        sendCommand(QStringLiteral("Input.dispatchMouseEvent"),
                    {{QStringLiteral("type"), QStringLiteral("mouseReleased")},
                     {QStringLiteral("x"), m_loginButtonX},
                     {QStringLiteral("y"), m_loginButtonY},
                     {QStringLiteral("button"), QStringLiteral("left")},
                     {QStringLiteral("clickCount"), 1}});
        return;
    case LoginStep::ReleasingLoginButton:
        m_resultCheckAttempts = 0;
        QTimer::singleShot(500, this, &KorailAuthController::checkLoginResult);
        return;
    case LoginStep::DismissingTransientError:
        m_step = LoginStep::ClosingTransientError;
        sendCommand(QStringLiteral("Input.dispatchMouseEvent"),
                    {{QStringLiteral("type"), QStringLiteral("mouseReleased")},
                     {QStringLiteral("x"), m_dialogButtonX},
                     {QStringLiteral("y"), m_dialogButtonY},
                     {QStringLiteral("button"), QStringLiteral("left")},
                     {QStringLiteral("clickCount"), 1}});
        return;
    case LoginStep::ClosingTransientError:
        m_dismissCheckAttempts = 0;
        QTimer::singleShot(100, this, &KorailAuthController::waitForTransientErrorDismissal);
        return;
    case LoginStep::WaitingForTransientErrorDismissal: {
        const bool stillOpen = result.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("value")).toBool();
        if (!stillOpen) {
            pressLoginButton();
            return;
        }
        if (++m_dismissCheckAttempts >= 10) {
            finish(tr("코레일 통신 오류 안내를 닫지 못했습니다. 열린 Chrome 탭을 확인하세요."), true);
            return;
        }
        QTimer::singleShot(100, this, &KorailAuthController::waitForTransientErrorDismissal);
        return;
    }
    case LoginStep::CheckingLogin: {
        const QJsonObject value = result.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("value")).toObject();
        if (value.value(QStringLiteral("transientError")).toBool()) {
            if (m_transientErrorRetryCount >= 1
                || !value.value(QStringLiteral("confirmX")).isDouble()
                || !value.value(QStringLiteral("confirmY")).isDouble()) {
                finish(tr("코레일 로그인 요청이 통신 오류로 다시 실패했습니다. 열린 Chrome 탭을 확인하세요."), true);
                return;
            }
            ++m_transientErrorRetryCount;
            m_dialogButtonX = value.value(QStringLiteral("confirmX")).toDouble();
            m_dialogButtonY = value.value(QStringLiteral("confirmY")).toDouble();
            emit statusChanged(tr("코레일 통신 오류 안내를 닫고 로그인 요청을 한 번 재시도합니다..."));
            m_step = LoginStep::DismissingTransientError;
            sendCommand(QStringLiteral("Input.dispatchMouseEvent"),
                        {{QStringLiteral("type"), QStringLiteral("mousePressed")},
                         {QStringLiteral("x"), m_dialogButtonX},
                         {QStringLiteral("y"), m_dialogButtonY},
                         {QStringLiteral("button"), QStringLiteral("left")},
                         {QStringLiteral("clickCount"), 1}});
            return;
        }
        if (value.value(QStringLiteral("loggedIn")).toBool()
            || !value.value(QStringLiteral("loginFormPresent")).toBool()) {
            finish(tr("코레일 로그인 완료를 감지했습니다."), false);
            return;
        }
        if (++m_resultCheckAttempts >= 20) {
            finish(tr("로그인 완료를 확인하지 못했습니다. 열린 Chrome 탭을 확인하세요."), true);
            return;
        }
        QTimer::singleShot(500, this, &KorailAuthController::checkLoginResult);
        return;
    }
    case LoginStep::Idle:
        return;
    }
}

void KorailAuthController::onCommandError(int id, const QString &message)
{
    if (m_inProgress && id == m_pendingCommandId) {
        finish(tr("Chrome CDP 명령을 실행하지 못했습니다: %1").arg(message), true);
    }
}

void KorailAuthController::waitForLoginForm()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    m_step = LoginStep::WaitingForLoginForm;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"),
                  QStringLiteral("(() => { const button = document.querySelector('#tab_memNum .btn_bn-depblue'); if (!document.querySelector('#id') || !document.querySelector('#password') || !button || button.disabled || document.readyState === 'loading') return false; const rect = button.getBoundingClientRect(); const style = getComputedStyle(button); return rect.width > 0 && rect.height > 0 && style.visibility !== 'hidden' && style.display !== 'none'; })()")},
                 {QStringLiteral("returnByValue"), true}});
}

void KorailAuthController::submitLogin()
{
    if (!m_inProgress) {
        return;
    }
    const QString expression = QStringLiteral(R"JS(
(() => {
    const input = document.querySelector('#id');
    if (!input) {
        throw new Error('Korail member-number input is unavailable.');
    }
    input.focus();
    input.select();
    return document.activeElement === input;
})()
)JS");
    m_step = LoginStep::FocusingMemberNumber;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"), expression}, {QStringLiteral("awaitPromise"), true},
                 {QStringLiteral("returnByValue"), true}, {QStringLiteral("userGesture"), true}});
}

void KorailAuthController::focusPasswordField()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    const QString expression = QStringLiteral(R"JS(
(() => {
    const input = document.querySelector('#password');
    if (!input) {
        throw new Error('Korail password input is unavailable.');
    }
    input.focus();
    input.select();
    return document.activeElement === input;
})()
)JS");
    m_step = LoginStep::FocusingPassword;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"), expression}, {QStringLiteral("awaitPromise"), true},
                 {QStringLiteral("returnByValue"), true}, {QStringLiteral("userGesture"), true}});
}

void KorailAuthController::pressLoginButton()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    m_step = LoginStep::PressingLoginButton;
    sendCommand(QStringLiteral("Input.dispatchMouseEvent"),
                {{QStringLiteral("type"), QStringLiteral("mousePressed")},
                 {QStringLiteral("x"), m_loginButtonX},
                 {QStringLiteral("y"), m_loginButtonY},
                 {QStringLiteral("button"), QStringLiteral("left")},
                 {QStringLiteral("clickCount"), 1}});
}

void KorailAuthController::waitForTransientErrorDismissal()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    m_step = LoginStep::WaitingForTransientErrorDismissal;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"),
                  QStringLiteral("Boolean(document.body && document.body.innerText.includes('통신 중 에러가 발생하였습니다'))")},
                 {QStringLiteral("returnByValue"), true}});
}

void KorailAuthController::checkLoginResult()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    m_step = LoginStep::CheckingLogin;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"),
                  QStringLiteral("(() => { const transientMessage = '통신 중 에러가 발생하였습니다'; const transientError = Boolean(document.body && document.body.innerText.includes(transientMessage)); const controls = Array.from(document.querySelectorAll('button, [role=button], input[type=button], input[type=submit]')); const confirm = transientError ? controls.find((element) => { const rect = element.getBoundingClientRect(); const style = getComputedStyle(element); const label = (element.textContent || element.value || '').trim(); return label === '확인' && rect.width > 0 && rect.height > 0 && style.visibility !== 'hidden' && style.display !== 'none'; }) : null; const rect = confirm && confirm.getBoundingClientRect(); return { loginFormPresent: Boolean(document.querySelector('#id') && document.querySelector('#password')), loggedIn: Array.from(document.querySelectorAll('a, button')).some((element) => (element.textContent || '').trim() === '로그아웃'), transientError, confirmX: rect ? rect.left + (rect.width / 2) : null, confirmY: rect ? rect.top + (rect.height / 2) : null }; })()")},
                 {QStringLiteral("returnByValue"), true}});
}

void KorailAuthController::finish(const QString &message, bool failed)
{
    if (!m_inProgress) {
        return;
    }
    m_inProgress = false;
    m_step = LoginStep::Idle;
    m_pendingCommandId = 0;
    m_sessionId.clear();
    m_memberNumber.clear();
    m_password.clear();
    m_cdpClient->close();
    emit statusChanged(message);
    if (failed) {
        emit loginFailed(message);
    } else {
        emit loginSucceeded();
    }
}

int KorailAuthController::sendCommand(const QString &method, const QJsonObject &parameters)
{
    m_pendingCommandId = m_cdpClient->sendCommand(method, parameters, m_sessionId);
    if (m_pendingCommandId == 0 && m_inProgress) {
        finish(tr("Chrome CDP 명령을 보낼 수 없습니다."), true);
    }
    return m_pendingCommandId;
}
