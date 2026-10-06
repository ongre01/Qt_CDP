#ifndef TRAININFO_H
#define TRAININFO_H

#include <QString>

struct TrainInfo
{
    QString trainType;
    QString trainNo;
    QString departure;
    QString departureTime;
    QString arrival;
    QString arrivalTime;
    QString duration;
    QString generalSeat;
    QString specialSeat;
    bool generalReservable = false;
    bool specialReservable = false;
};

#endif // TRAININFO_H
