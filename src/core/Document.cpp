#include "core/Document.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <utility>

namespace ketplus {

Document::LoadResult Document::load(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, file.errorString(), {}};
    }

    QByteArray content = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return {false, file.errorString(), {}};
    }

    filePath_ = QFileInfo(filePath).absoluteFilePath();
    modified_ = false;
    return {true, {}, std::move(content)};
}

Document::Result Document::save(const QByteArray& content) {
    if (isUntitled()) {
        return {false, QStringLiteral("Document has no file path")};
    }
    return saveAs(filePath_, content);
}

Document::Result Document::saveAs(const QString& filePath, const QByteArray& content) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return {false, file.errorString()};
    }

    if (file.write(content) != content.size()) {
        return {false, file.errorString()};
    }
    if (!file.commit()) {
        return {false, file.errorString()};
    }

    filePath_ = QFileInfo(filePath).absoluteFilePath();
    modified_ = false;
    return {true, {}};
}

const QString& Document::filePath() const noexcept {
    return filePath_;
}

bool Document::isUntitled() const noexcept {
    return filePath_.isEmpty();
}

bool Document::isModified() const noexcept {
    return modified_;
}

QString Document::displayName() const {
    return isUntitled() ? QStringLiteral("Untitled") : QFileInfo(filePath_).fileName();
}

void Document::setModified(const bool modified) noexcept {
    modified_ = modified;
}

} // namespace ketplus
