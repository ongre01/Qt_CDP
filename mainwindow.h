#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void startChromeForCdp();
    void checkStartedChromeEndpoint();
    void setBusy(bool busy);
    void showStatus(const QString &message, bool isError = false);
    QUrl debuggerVersionUrl(QString *errorMessage = nullptr) const;

    Ui::MainWindow *ui;
    QNetworkAccessManager m_networkManager;
    QTimer m_cdpReadyTimer;
    int m_cdpReadyAttempts = 0;
    bool m_startupRequestInFlight = false;
};
#endif // MAINWINDOW_H
