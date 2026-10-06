#include "korailauthcontroller.h"

#include "../cdp/cdpclient.h"

#include <QJsonDocument>
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
    m_sessionId.clear();
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
                {{QStringLiteral("url"), QStringLiteral("https://www.korail.com/ticket/login")}});
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
    case LoginStep::SubmittingLogin:
        m_resultCheckAttempts = 0;
        QTimer::singleShot(500, this, &KorailAuthController::checkLoginResult);
        return;
    case LoginStep::CheckingLogin: {
        const QJsonObject value = result.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("value")).toObject();
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
                  QStringLiteral("Boolean(document.querySelector('#id') && document.querySelector('#password') && document.querySelector('#tab_memNum .btn_bn-depblue'))")},
                 {QStringLiteral("returnByValue"), true}});
}

void KorailAuthController::submitLogin()
{
    if (!m_inProgress) {
        return;
    }
    const QJsonObject credentials {{QStringLiteral("memberNumber"), m_memberNumber},
                                  {QStringLiteral("password"), m_password}};
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
)JS").arg(QString::fromUtf8(QJsonDocument(credentials).toJson(QJsonDocument::Compact)));
    m_step = LoginStep::SubmittingLogin;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"), expression}, {QStringLiteral("awaitPromise"), true},
                 {QStringLiteral("returnByValue"), true}, {QStringLiteral("userGesture"), true}});
}

void KorailAuthController::checkLoginResult()
{
    if (!m_inProgress || m_sessionId.isEmpty()) {
        return;
    }
    m_step = LoginStep::CheckingLogin;
    sendCommand(QStringLiteral("Runtime.evaluate"),
                {{QStringLiteral("expression"),
                  QStringLiteral("(() => ({ loginFormPresent: Boolean(document.querySelector('#id') && document.querySelector('#password')), loggedIn: Array.from(document.querySelectorAll('a, button')).some((element) => (element.textContent || '').trim() === '로그아웃') }))()")},
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
