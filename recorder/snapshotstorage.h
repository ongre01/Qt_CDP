#ifndef SNAPSHOTSTORAGE_H
#define SNAPSHOTSTORAGE_H

#include <QJsonObject>
#include <QString>

class SnapshotStorage
{
public:
    explicit SnapshotStorage(const QString &directory = defaultDirectory());

    void setDirectory(const QString &directory);
    QString directory() const;
    bool prepare(const QString &captureId) const;
    bool save(const QJsonObject &snapshot);
    bool save(const QString &captureId, const QString &url, const QString &title,
              const QJsonObject &snapshot);

    static QString defaultDirectory();
    static bool isRecordablePageUrl(const QString &url);

private:
    QJsonObject redact(const QJsonObject &snapshot) const;
    bool writeJson(const QString &fileName, const QJsonObject &object) const;
    void appendManifest(const QJsonObject &entry) const;

    QString m_directory;
    int m_nextSequence = 1;
};

#endif // SNAPSHOTSTORAGE_H
