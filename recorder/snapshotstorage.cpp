#include "snapshotstorage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

SnapshotStorage::SnapshotStorage(const QString &directory)
    : m_directory(directory)
{
}

void SnapshotStorage::setDirectory(const QString &directory)
{
    m_directory = directory;
}

QString SnapshotStorage::directory() const
{
    return m_directory;
}

bool SnapshotStorage::prepare(const QString &captureId) const
{
    return QDir().mkpath(QDir(m_directory).filePath(captureId));
}

bool SnapshotStorage::save(const QJsonObject &snapshot)
{
    const QString captureId = QDateTime::currentDateTimeUtc()
                                  .toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"))
                              + QStringLiteral("-%1").arg(m_nextSequence++);
    return save(captureId, {}, {}, snapshot);
}

bool SnapshotStorage::save(const QString &captureId, const QString &url, const QString &title,
                           const QJsonObject &snapshot)
{
    const QString captureDirectory = QDir(m_directory).filePath(captureId);
    const QJsonObject document {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("captureId"), captureId},
        {QStringLiteral("capturedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("domSnapshot"), redact(snapshot)}
    };
    if (!writeJson(QDir(captureDirectory).filePath(QStringLiteral("dom-snapshot.json")), document)) {
        return false;
    }
    appendManifest({{QStringLiteral("schemaVersion"), 1},
                    {QStringLiteral("captureId"), captureId},
                    {QStringLiteral("capturedAt"), document.value(QStringLiteral("capturedAt"))},
                    {QStringLiteral("url"), url},
                    {QStringLiteral("title"), title},
                    {QStringLiteral("domSnapshotFile"),
                     QDir(captureId).filePath(QStringLiteral("dom-snapshot.json"))}});
    return true;
}

QString SnapshotStorage::defaultDirectory()
{
    QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDirectory.isEmpty()) {
        baseDirectory = QDir::tempPath();
    }
    return QDir(baseDirectory).filePath(QStringLiteral("page-captures"));
}

bool SnapshotStorage::isRecordablePageUrl(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme != QStringLiteral("devtools") && scheme != QStringLiteral("chrome")
           && scheme != QStringLiteral("edge");
}

QJsonObject SnapshotStorage::redact(const QJsonObject &source) const
{
    QJsonObject snapshot = source;
    QJsonArray strings = snapshot.value(QStringLiteral("strings")).toArray();
    const auto redactStringAt = [&strings](int index) {
        if (index >= 0 && index < strings.size()) {
            strings[index] = QStringLiteral("[REDACTED]");
        }
    };
    const auto stringAt = [&strings](int index) {
        return index >= 0 && index < strings.size() ? strings.at(index).toString() : QString();
    };

    QJsonArray documents = snapshot.value(QStringLiteral("documents")).toArray();
    for (int documentIndex = 0; documentIndex < documents.size(); ++documentIndex) {
        QJsonObject document = documents.at(documentIndex).toObject();
        QJsonObject nodes = document.value(QStringLiteral("nodes")).toObject();
        const auto removeInputValues = [&nodes, &redactStringAt](const QString &field) {
            if (!nodes.contains(field)) {
                return;
            }
            const QJsonArray values = nodes.value(field).toObject().value(QStringLiteral("value")).toArray();
            for (const QJsonValue &value : values) {
                redactStringAt(value.toInt(-1));
            }
            nodes.remove(field);
        };
        removeInputValues(QStringLiteral("inputValue"));
        removeInputValues(QStringLiteral("textValue"));

        const QJsonArray nodeNames = nodes.value(QStringLiteral("nodeName")).toArray();
        const QJsonArray attributes = nodes.value(QStringLiteral("attributes")).toArray();
        for (int nodeIndex = 0; nodeIndex < attributes.size() && nodeIndex < nodeNames.size(); ++nodeIndex) {
            if (stringAt(nodeNames.at(nodeIndex).toInt(-1)).toLower() != QStringLiteral("input")) {
                continue;
            }
            const QJsonArray attributeIndexes = attributes.at(nodeIndex).toArray();
            for (int attributeIndex = 0; attributeIndex + 1 < attributeIndexes.size(); attributeIndex += 2) {
                const QString attributeName = stringAt(attributeIndexes.at(attributeIndex).toInt(-1)).toLower();
                if (attributeName == QStringLiteral("value")) {
                    redactStringAt(attributeIndexes.at(attributeIndex + 1).toInt(-1));
                }
            }
        }
        document.insert(QStringLiteral("nodes"), nodes);
        documents[documentIndex] = document;
    }
    snapshot.insert(QStringLiteral("strings"), strings);
    snapshot.insert(QStringLiteral("documents"), documents);
    return snapshot;
}

bool SnapshotStorage::writeJson(const QString &fileName, const QJsonObject &object) const
{
    if (!QDir().mkpath(QFileInfo(fileName).absolutePath())) {
        return false;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
        return false;
    }
    return file.commit();
}

void SnapshotStorage::appendManifest(const QJsonObject &entry) const
{
    QFile file(QDir(m_directory).filePath(QStringLiteral("manifest.jsonl")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
    file.write("\n");
}
