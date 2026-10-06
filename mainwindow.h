#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
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
class QTableWidgetItem;
class SnapshotStorage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void startChromeForCdp();
    void startKorailAutoLogin();
    void togglePageRecording();
    void openSnapshotDirectory();
    void startTrainInfoMonitoring();
    void startTrainRefreshMacro();
    void stopTrainRefreshMacro(const QString &message = QString());
    void onDomSnapshotCaptured(const QJsonObject &snapshot, const QString &sessionId);
    void updateTrainInfoTable(const QList<TrainInfo> &trains);
    void clearTrainInfoTable();
    void onTrainInfoItemChanged(QTableWidgetItem *item);
    QList<TrainInfo> selectedTrains() const;
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
    AutoBookingController *m_autoBookingController;
    QList<TrainInfo> m_currentTrains;
    QSet<QString> m_selectedTrainKeys;
    QString m_trainInfoSessionId;
    bool m_updatingTrainInfoTable = false;
};

#endif // MAINWINDOW_H
