#include "state/JobJournalFile.h"

#include "delivery/DeliveryRecords.h"
#include "state/StateDirectory.h"

#include <QJsonDocument>

namespace ketplus::runner::app {

namespace {

const QString fileName = QStringLiteral("journal.json");

} // namespace

JobJournalFile::Loaded JobJournalFile::load() const {
    Loaded loaded;
    const StateDirectory::ReadResult read = m_directory.read(fileName);
    if (read.status == StateDirectory::ReadResult::Status::Missing) {
        loaded.ok = true;
        return loaded;
    }
    if (read.status == StateDirectory::ReadResult::Status::Invalid) {
        loaded.error = read.error;
        return loaded;
    }
    const QJsonDocument document = QJsonDocument::fromJson(read.data);
    loaded.record =
        document.isObject() ? delivery::journalFromJson(document.object()) : std::nullopt;
    if (!loaded.record) {
        loaded.error = QStringLiteral("%1/%2 is corrupt; it was left as it is")
                           .arg(m_directory.path(), fileName);
        return loaded;
    }
    loaded.ok = true;
    return loaded;
}

bool JobJournalFile::write(const delivery::JournalRecord& record) {
    return m_directory.write(
        fileName, QJsonDocument(delivery::journalToJson(record)).toJson(QJsonDocument::Compact),
        &m_error);
}

bool JobJournalFile::clear() { return m_directory.remove(fileName, &m_error); }

} // namespace ketplus::runner::app
