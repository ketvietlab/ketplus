#pragma once

#include <QByteArray>
#include <QString>

namespace ketplus {

class Document final {
public:
    struct Result {
        bool ok{false};
        QString error;
    };

    struct LoadResult {
        bool ok{false};
        QString error;
        QByteArray content;
    };

    [[nodiscard]] LoadResult load(const QString& filePath);
    [[nodiscard]] Result save(const QByteArray& content);
    [[nodiscard]] Result saveAs(const QString& filePath, const QByteArray& content);

    [[nodiscard]] const QString& filePath() const noexcept;
    [[nodiscard]] bool isUntitled() const noexcept;
    [[nodiscard]] bool isModified() const noexcept;
    [[nodiscard]] QString displayName() const;

    void setModified(bool modified) noexcept;

private:
    QString filePath_;
    bool modified_{false};
};

} // namespace ketplus
