#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>

namespace ketplus::remote {

class LargeEventReassembler final {
  public:
    explicit LargeEventReassembler(const QJsonObject& reference);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isComplete() const;
    [[nodiscard]] qint64 nextOffset() const;
    [[nodiscard]] QString transferId() const;
    [[nodiscard]] QString eventType() const;
    [[nodiscard]] QString itemId() const;
    [[nodiscard]] QString digestSha256() const;
    [[nodiscard]] qint64 totalBytes() const;
    [[nodiscard]] qint64 revision() const;
    bool accept(const QJsonObject& fragment, QString* error = nullptr);
    [[nodiscard]] QJsonObject payload(QString* error = nullptr) const;

  private:
    QString transferId_;
    QString eventType_;
    QString itemId_;
    QString digestSha256_;
    qint64 totalBytes_{-1};
    qint64 revision_{0};
    qint64 nextOffset_{0};
    QByteArray bytes_;
    QHash<qint64, QByteArray> accepted_;
    bool valid_{false};
    bool complete_{false};
};

} // namespace ketplus::remote
