#ifndef AUTOBOOKINGCONTROLLER_H
#define AUTOBOOKINGCONTROLLER_H

#include "traininfo.h"

#include <QObject>
#include <QJsonObject>
#include <QList>

class CdpClient;
class PageRecorder;
class QTimer;

class AutoBookingController : public QObject
{
    Q_OBJECT

public:
    explicit AutoBookingController(CdpClient *cdpClient, PageRecorder *pageRecorder,
                                   QObject *parent = nullptr);

    void start();
    void stop();
    bool isRunning() const;
    void setRefreshIntervalSeconds(int seconds);
    void setAutoBookWhenAvailable(bool enabled);
    void setSelectedTrains(const QList<TrainInfo> &trains);
    void setTrainInfoSession(const QString &sessionId);
    void setTrainInfoContext(const QString &sessionId, const QList<TrainInfo> &trains);

signals:
    void bookingStarted();
    void bookingSucceeded();
    void bookingFailed(const QString &reason);
    void runningChanged(bool running);
    void statusChanged(const QString &message);

private:
    enum class RequestType {
        SelectSeat,
        SelectSeatMousePressed,
        SelectSeatMouseReleased,
        ConfirmBooking,
        ConfirmMousePressed,
        ConfirmMouseReleased,
        DismissDialog,
        DismissDialogMousePressed,
        DismissDialogMouseReleased
    };

    void update();
    void refreshSelectedTrainPage();
    const TrainInfo *reservableSelectedTrain() const;
    void startAutoBooking(const TrainInfo &train);
    void continueWithConfirmation(const QString &sessionId);
    void continueWithInformationalDialogs(const QString &sessionId);
    void onCommandResult(int id, const QJsonObject &result);
    void onCommandError(int id, const QString &message);
    void finishBookingSuccessfully(const QString &message);
    void resetBookingState();
    void failBooking(const QString &message);
    int sendCommand(const QString &method, const QJsonObject &parameters,
                    const QString &sessionId);

    CdpClient *m_cdpClient;
    PageRecorder *m_pageRecorder;
    QTimer *m_refreshTimer;
    QList<TrainInfo> m_selectedTrains;
    QString m_sessionId;
    bool m_running = false;
    bool m_autoBookWhenAvailable = false;
    bool m_bookingInProgress = false;
    QString m_seatType;
    double m_clickX = 0.0;
    double m_clickY = 0.0;
    int m_confirmationAttempts = 0;
    int m_dialogAttempts = 0;
    int m_dialogClicks = 0;
    bool m_dialogDomFallbackUsed = false;
    QHash<int, RequestType> m_requests;
};

#endif // AUTOBOOKINGCONTROLLER_H
