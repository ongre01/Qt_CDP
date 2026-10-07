#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QSet>

#include "korail/traininfo.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class AutoBookingController;
class CdpClient;
class KorailAuthController;
class PageRecorder;
class QJsonObject;
class QNetworkAccessManager;
class QTableWidget;
class QTableWidgetItem;
class SnapshotStorage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    struct TrainInfoTab {
        int number = 0;
        QList<TrainInfo> trains;
        AutoBookingController *controller = nullptr;
        QTableWidget *table = nullptr;
    };

    void startChromeForCdp();
    void startKorailAutoLogin();
    void togglePageRecording();
    void openSnapshotDirectory();
    void startTrainInfoMonitoring();
    void startTrainRefreshMacro();
    void stopTrainRefreshMacro(const QString &message = QString());
    void onDomSnapshotCaptured(const QJsonObject &snapshot, const QString &sessionId);
    void updateTrainInfoTable();
    void clearTrainInfoTable();
    void removeTrainInfoTab(const QString &sessionId);
    void onTrainInfoItemChanged(const QString &sessionId, QTableWidgetItem *item);
    QList<TrainInfo> selectedTrains(const QString &sessionId) const;
    QString trainSelectionKey(const QString &sessionId, const TrainInfo &train) const;
    QTableWidget *createTrainInfoTable(const QString &sessionId);
    AutoBookingController *createAutoBookingController(const QString &sessionId,
                                                       const QString &label);
    void updateTrainRefreshMacroUi();
    void sendBookingNotification(const QString &tab);
    void updatePageRecordingUi(bool active);
    void setBusy(bool busy);
    void showStatus(const QString &message, bool isError = false);

    Ui::MainWindow *ui;
    CdpClient *m_startupCdpClient;
    CdpClient *m_authCdpClient;
    CdpClient *m_recorderCdpClient;
    SnapshotStorage *m_snapshotStorage;
    KorailAuthController *m_authController;
    PageRecorder *m_pageRecorder;
    QNetworkAccessManager *m_notificationNetworkManager;
    QHash<QString, TrainInfoTab> m_trainInfoTabs;
    QSet<QString> m_selectedTrainKeys;
    int m_nextTrainInfoTabNumber = 1;
    bool m_trainRefreshMacroRunning = false;
    bool m_updatingTrainInfoTable = false;
};

#endif // MAINWINDOW_H
