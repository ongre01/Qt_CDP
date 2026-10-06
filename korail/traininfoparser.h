#ifndef TRAININFOPARSER_H
#define TRAININFOPARSER_H

#include "traininfo.h"

#include <QJsonObject>
#include <QList>

class TrainInfoParser
{
public:
    static QList<TrainInfo> parse(const QJsonObject &domSnapshot);
    static QList<TrainInfo> parse(const QJsonObject &domSnapshot, bool *isTicketReservationPage);
    static QString selectionKey(const TrainInfo &train);
    static bool isReservableSeatText(const QString &seatText);
};

#endif // TRAININFOPARSER_H
