#include "traininfoparser.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

namespace {

QString normalizedText(const QString &text)
{
    QString result = text;
    result.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return result.trimmed();
}

} // namespace

QList<TrainInfo> TrainInfoParser::parse(const QJsonObject &domSnapshot)
{
    return parse(domSnapshot, nullptr);
}

QList<TrainInfo> TrainInfoParser::parse(const QJsonObject &snapshot, bool *isTicketReservationPage)
{
    if (isTicketReservationPage) {
        *isTicketReservationPage = false;
    }
    const QJsonArray documents = snapshot.value(QStringLiteral("documents")).toArray();
    const QJsonArray strings = snapshot.value(QStringLiteral("strings")).toArray();
    if (documents.isEmpty() || strings.isEmpty()) {
        return {};
    }

    const QJsonObject nodes = documents.first().toObject().value(QStringLiteral("nodes")).toObject();
    const QJsonArray nodeNames = nodes.value(QStringLiteral("nodeName")).toArray();
    const QJsonArray nodeValues = nodes.value(QStringLiteral("nodeValue")).toArray();
    const QJsonArray parentIndexes = nodes.value(QStringLiteral("parentIndex")).toArray();
    const QJsonArray attributes = nodes.value(QStringLiteral("attributes")).toArray();
    if (nodeNames.isEmpty() || parentIndexes.size() != nodeNames.size()) {
        return {};
    }

    const auto stringAt = [&strings](int index) {
        return index >= 0 && index < strings.size() ? strings.at(index).toString() : QString();
    };
    const auto nodeName = [&nodeNames, &stringAt](int index) {
        return stringAt(nodeNames.at(index).toInt(-1)).toLower();
    };
    const auto attributeValue = [&attributes, &stringAt](int nodeIndex, const QString &attributeName) {
        if (nodeIndex < 0 || nodeIndex >= attributes.size()) {
            return QString();
        }
        const QJsonArray pairs = attributes.at(nodeIndex).toArray();
        for (int pairIndex = 0; pairIndex + 1 < pairs.size(); pairIndex += 2) {
            if (stringAt(pairs.at(pairIndex).toInt(-1)).compare(attributeName, Qt::CaseInsensitive) == 0) {
                return stringAt(pairs.at(pairIndex + 1).toInt(-1));
            }
        }
        return QString();
    };

    QVector<QVector<int>> children(nodeNames.size());
    for (int index = 0; index < parentIndexes.size(); ++index) {
        const int parentIndex = parentIndexes.at(index).toInt(-1);
        if (parentIndex >= 0 && parentIndex < children.size()) {
            children[parentIndex].append(index);
        }
    }
    const auto textForSubtree = [&children, &nodeName, &nodeValues, &stringAt](int root) {
        QStringList fragments;
        QVector<int> pending {root};
        while (!pending.isEmpty()) {
            const int index = pending.takeLast();
            if (nodeName(index) == QStringLiteral("#text")) {
                const int valueIndex = index < nodeValues.size() ? nodeValues.at(index).toInt(-1) : -1;
                fragments.append(stringAt(valueIndex));
            }
            const QVector<int> &childNodes = children.at(index);
            for (auto child = childNodes.crbegin(); child != childNodes.crend(); ++child) {
                pending.append(*child);
            }
        }
        return normalizedText(fragments.join(QLatin1Char(' ')));
    };
    const auto textForClassToken = [&children, &attributeValue, &textForSubtree](int root,
                                                                                   const QString &classToken) {
        QVector<int> pending {root};
        while (!pending.isEmpty()) {
            const int index = pending.takeLast();
            const QStringList classes = attributeValue(index, QStringLiteral("class"))
                                            .split(QRegularExpression(QStringLiteral("\\s+")),
                                                   Qt::SkipEmptyParts);
            if (classes.contains(classToken, Qt::CaseInsensitive)) {
                return textForSubtree(index);
            }
            const QVector<int> &childNodes = children.at(index);
            for (auto child = childNodes.crbegin(); child != childNodes.crend(); ++child) {
                pending.append(*child);
            }
        }
        return QString();
    };

    int bodyIndex = -1;
    for (int index = 0; index < nodeNames.size(); ++index) {
        if (nodeName(index) == QStringLiteral("body")) {
            bodyIndex = index;
            break;
        }
    }
    const QString pageText = bodyIndex >= 0 ? textForSubtree(bodyIndex) : textForSubtree(0);
    const bool reservationPage = QRegularExpression(
        QStringLiteral("승차권\\s*예매|열차\\s*(조회|예매)|출발역\\s*.*도착역"))
                                     .match(pageText)
                                     .hasMatch();
    if (isTicketReservationPage) {
        *isTicketReservationPage = reservationPage;
    }
    if (!reservationPage) {
        return {};
    }

    const QRegularExpression timePattern(QStringLiteral("(?:[01]?\\d|2[0-3]):[0-5]\\d"));
    const QRegularExpression trainTypePattern(
        QStringLiteral("KTX(?:-산천)?|SRT|ITX-?(?:새마을|마음)|새마을호|무궁화호|누리로|통근열차"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression trainNumberPattern(QStringLiteral("(?:열차\\s*번호\\s*)?(\\d{3,5})\\s*호?"));
    const QRegularExpression stationPairPattern(
        QStringLiteral("([가-힣A-Za-z0-9]+)\\s*(?:역)?\\s*(?:→|->|~|-)\\s*([가-힣A-Za-z0-9]+)\\s*(?:역)?"));
    const QRegularExpression availabilityPattern(QStringLiteral("예매|예약|좌석|매진|입석|잔여|특실|일반실"));
    QSet<QString> seenRows;
    QList<TrainInfo> trains;

    for (int index = 0; index < nodeNames.size() && trains.size() < 200; ++index) {
        const QString tagName = nodeName(index);
        const bool isTrainListItem = tagName == QStringLiteral("li")
            && attributeValue(index, QStringLiteral("class")).contains(QStringLiteral("tckList"), Qt::CaseInsensitive);
        if (tagName != QStringLiteral("tr") && !isTrainListItem) {
            continue;
        }
        const QString rowText = textForSubtree(index);
        const QString titleText = isTrainListItem ? textForClassToken(index, QStringLiteral("tit_box")) : rowText;
        const QString travelText = isTrainListItem ? textForClassToken(index, QStringLiteral("data_box")) : rowText;
        const QString generalSeat = isTrainListItem ? textForClassToken(index, QStringLiteral("gen")) : rowText;
        const QString specialSeat = isTrainListItem ? textForClassToken(index, QStringLiteral("spe")) : QString();
        QRegularExpressionMatchIterator timeMatches = timePattern.globalMatch(travelText);
        QStringList times;
        while (timeMatches.hasNext()) {
            times.append(timeMatches.next().captured(0));
        }
        if (times.size() < 2 || !availabilityPattern.match(rowText).hasMatch()) {
            continue;
        }
        const QRegularExpressionMatch typeMatch = trainTypePattern.match(titleText);
        const QRegularExpressionMatch numberMatch = trainNumberPattern.match(titleText);
        const QRegularExpressionMatch stationMatch = stationPairPattern.match(travelText);
        const QRegularExpressionMatch durationMatch = QRegularExpression(
            QStringLiteral("소요시간\\s*:\\s*(.+)$")).match(travelText);
        TrainInfo train;
        train.trainType = typeMatch.hasMatch() ? typeMatch.captured(0) : QString();
        train.trainNo = numberMatch.hasMatch() ? numberMatch.captured(1) : QString();
        train.departure = stationMatch.hasMatch() ? stationMatch.captured(1) : QString();
        train.departureTime = times.at(0);
        train.arrival = stationMatch.hasMatch() ? stationMatch.captured(2) : QString();
        train.arrivalTime = times.at(1);
        train.duration = durationMatch.hasMatch() ? durationMatch.captured(1).trimmed() : QString();
        train.generalSeat = generalSeat;
        train.specialSeat = specialSeat;
        train.generalReservable = isReservableSeatText(generalSeat);
        train.specialReservable = isReservableSeatText(specialSeat);
        const QString signature = QStringList {train.trainType, train.trainNo, train.departure,
                                                train.departureTime, train.arrival, train.arrivalTime,
                                                train.duration, train.generalSeat, train.specialSeat}
                                      .join(QLatin1Char('|'));
        if (!seenRows.contains(signature)) {
            seenRows.insert(signature);
            trains.append(train);
        }
    }
    return trains;
}

QString TrainInfoParser::selectionKey(const TrainInfo &train)
{
    return QStringList {train.trainType, train.trainNo, train.departure, train.departureTime,
                        train.arrival, train.arrivalTime}.join(QChar(0x1f));
}

bool TrainInfoParser::isReservableSeatText(const QString &seatText)
{
    const QString text = normalizedText(seatText);
    const bool indicatesReservation = text.contains(QStringLiteral("예매"))
        || text.contains(QStringLiteral("예약"));
    const bool indicatesFare = QRegularExpression(
        QStringLiteral("\\b\\d{1,3}(?:,\\d{3})*\\s*원")).match(text).hasMatch();
    const bool indicatesUnavailability = text.contains(QStringLiteral("매진"))
        || text.contains(QStringLiteral("없음")) || text.contains(QStringLiteral("불가"))
        || text.contains(QStringLiteral("대기"));
    return (indicatesReservation || indicatesFare) && !indicatesUnavailability;
}
