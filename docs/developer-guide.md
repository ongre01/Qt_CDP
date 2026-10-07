# 개발자 가이드

## 기술 구성과 빌드

프로젝트는 qmake 프로젝트 파일 [`Qt_CDP.pro`](../Qt_CDP.pro)를 사용합니다. 필요한 Qt 모듈은 `widgets`, `network`, `websockets`이고, C++17을 사용합니다.

Qt Creator에서는 `.pro` 파일을 열고 위 모듈을 제공하는 Kit을 선택한 뒤 빌드합니다. 명령줄에서는 선택한 Qt Kit의 `qmake Qt_CDP.pro`로 빌드 파일을 생성한 다음, 해당 Kit의 빌드 도구를 실행합니다. 도구 이름과 명령은 MSVC·MinGW 등 Kit에 따라 달라집니다.

프로젝트 파일에는 자동화된 테스트 대상이 선언되어 있지 않습니다. 변경 후에는 아래의 수동 검증 항목을 수행하는 것이 현재 기준입니다.

## 소스 구조

| 경로 | 책임 |
| --- | --- |
| `main.cpp` | `QApplication`과 `MainWindow` 생성, 이벤트 루프 시작 |
| `mainwindow.*` | 화면 이벤트 처리, 컴포넌트 조합, 탭별 열차 목록·매크로 제어, 완료 알림 |
| `cdp/cdpclient.*` | Chrome 시작, `/json/version` 조회, WebSocket 연결, CDP 명령·이벤트 송수신 |
| `korail/korailauthcontroller.*` | CDP 기반 로그인 상태 전이와 로그인 완료 확인 |
| `korail/traininfoparser.*` | DOM 스냅샷에서 열차·좌석 정보를 파싱하고 선택 키 생성 |
| `korail/autobookingcontroller.*` | 탭별 새로고침·예약 가능 판정·자동 예매 절차 |
| `recorder/pagerecorder.*` | 페이지 target 자동 연결, DOM 스냅샷 요청, 세션 수명 관리 |
| `recorder/snapshotstorage.*` | 스냅샷 정제와 원자적 JSON 저장, 매니페스트 추가 |
| `mainwindow.ui` | Qt Designer 기반 UI 정의 |

## 런타임 구조

`MainWindow`는 역할을 분리한 세 개의 `CdpClient` 인스턴스를 만듭니다. 시작용 클라이언트는 Chrome 프로세스와 준비 여부만 담당하고, 로그인용 클라이언트와 기록용 클라이언트는 각각 독립 WebSocket CDP 연결을 사용합니다.

```mermaid
flowchart TD
    UI[MainWindow / Qt Widgets]
    UI --> Startup[CdpClient: Chrome 시작]
    Startup --> Endpoint[/json/version]
    Endpoint --> Chrome[Local Chrome CDP]
    UI --> Auth[CdpClient + KorailAuthController]
    Auth <--> Chrome
    UI --> Recorder[CdpClient + PageRecorder]
    Recorder <--> Chrome
    Recorder --> Parser[TrainInfoParser]
    Parser --> Tabs[탭별 열차 테이블]
    Tabs --> Booking[AutoBookingController]
    Booking <--> Chrome
    Recorder --> Storage[SnapshotStorage]
    Storage --> Files[manifest.jsonl / dom-snapshot.json]
    UI --> Notify[ntfy.sh/ktx]
```

### Chrome과 CDP 연결

`CdpClient::debuggerVersionUrl()`은 입력값을 HTTP(S) URL로 검증하고, 포트가 없으면 `9222`, 경로는 `/json/version`으로 정규화합니다. `startChrome()`은 로컬 호스트 URL만 허용하고 Chrome을 별도 프로필과 원격 디버깅 포트로 시작합니다. 이후 `/json/version` 응답의 WebSocket 디버거 URL을 사용해 CDP WebSocket에 연결합니다.

`sendCommand()`는 증가하는 명령 ID, `method`, 선택적 `params`, 선택적 `sessionId`를 JSON으로 전송합니다. 결과와 오류, 이벤트는 Qt signal로 상위 컨트롤러에 전달됩니다.

### 페이지 기록과 열차 목록

`PageRecorder`는 CDP 연결 후 `Target.setDiscoverTargets`, `Target.setAutoAttach`, `Target.getTargets`로 page target을 탐색·연결합니다. 연결된 페이지 세션에서 `DOMSnapshot.captureSnapshot`을 요청하고 `snapshotCaptured` 신호를 발생시킵니다.

`MainWindow::onDomSnapshotCaptured()`는 스냅샷을 `TrainInfoParser`에 전달해 코레일 열차 조회 페이지인지와 열차 목록을 판정합니다. 조회 페이지마다 열차 테이블과 `AutoBookingController`를 하나씩 유지합니다. 따라서 여러 브라우저 탭의 선택 열차를 병렬로 제어할 수 있습니다. 실행 중에는 각 조회 탭의 DOM 스냅샷을 SHA-256 지문으로 비교해 변화를 감지합니다. 변화가 없으면 `MainWindow`의 단일 타이머가 설정 시간 뒤 ntfy 알림을 한 번 보내며, 다음 변화가 감지될 때까지 같은 구간에서 중복 전송하지 않습니다.

`AutoBookingController`는 실행 중 선택 열차에 예약 가능 좌석이 없으면 해당 페이지 세션으로 `Page.reload`를 전송합니다. 예약 가능 좌석을 찾으면 자동 예매 옵션에 따라 멈춰 알리거나, CDP의 `Runtime.evaluate`와 입력 이벤트를 사용해 예매 절차를 진행합니다.

## 스냅샷 저장 형식과 개인정보 처리

저장 시 `SnapshotStorage`는 UTC 시간 기반 capture ID별 디렉터리를 준비하고, `QSaveFile`로 `dom-snapshot.json`을 원자적으로 기록합니다. 캡처 메타데이터는 `manifest.jsonl`에 JSON Lines 형식으로 추가합니다.

저장 직전에 `SnapshotStorage::redact()`가 `inputValue`, `textValue`, 그리고 `input` 요소의 `value` 속성이 참조하는 문자열을 `[REDACTED]`로 바꿉니다. 이 처리는 입력·textarea 값 노출을 줄이지만 전체 DOM의 다른 텍스트나 `manifest.jsonl`의 URL·제목까지 제거하지는 않습니다.

스냅샷 스키마의 현재 버전은 `1`입니다. 소비 도구는 알 수 없는 필드를 보존하고 `schemaVersion`을 검사하도록 구현하는 편이 안전합니다.

## 변경 시 점검할 흐름

1. **Chrome 시작**: 잘못된 실행 파일, 빈 사용자 데이터 경로, 원격 CDP 주소가 각각 오류로 처리되는지 확인합니다.
2. **CDP 연결**: 로컬 Chrome을 시작하거나 연결한 뒤 `/json/version`과 WebSocket 연결이 성공하는지 확인합니다.
3. **로그인**: 실제 계정을 사용하기 전에 테스트 환경에서 실패·중단·연결 해제 시 UI가 busy 상태에서 복구되는지 확인합니다.
4. **페이지 기록**: 입력 필드와 textarea가 있는 페이지를 기록한 뒤 생성된 JSON에 원문 값 대신 `[REDACTED]`가 저장되는지 확인합니다.
5. **열차 정보·매크로**: 조회 페이지를 여러 탭으로 열고 탭 생성·선택 유지·탭 닫힘·매크로 중지를 확인합니다. 설정한 무변화 시간이 지난 뒤 ntfy 요청이 한 번만 전송되는지와, DOM 변화 후 다음 감시 구간이 다시 시작되는지도 확인합니다.
6. **자동 예매**: 실제 예약이나 결제를 유발할 수 있으므로, 변경 검증 시에는 안전한 계정·환경과 명시적인 운영 승인 없이 활성화하지 않습니다.

## 유지보수 유의사항

- 코레일 페이지 선택자와 문구 의존성은 `KorailAuthController`, `TrainInfoParser`, `AutoBookingController`에 집중되어 있습니다. 서비스 화면 변경 시 이 세 영역을 함께 점검합니다.
- `PageRecorder`가 관리하는 target ID와 session ID의 수명은 다릅니다. target detach 이벤트에서는 테이블과 컨트롤러가 정리되는지 확인합니다.
- 로그인 정보는 현재 사용자 범위의 QSettings INI에 저장됩니다. UI 설명을 변경하거나 보안 정책을 강화할 때는 이 구현과 문서를 함께 갱신합니다.
- 완료 알림은 현재 `https://ntfy.sh/ktx`로 고정되어 있습니다. 배포용으로는 사용자별 설정, 인증, 실패 처리, 개인정보 정책을 설계한 뒤 교체하는 것이 필요합니다.
