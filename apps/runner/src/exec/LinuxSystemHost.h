#pragma once

#include "exec/LinuxPolicy.h"

#include <QObject>

// The machine the runner runs on, as the Linux policy reads it.
namespace ketplus::runner::exec {

class LinuxSystemHost final : public LinuxHost {
  public:
    [[nodiscard]] uint effectiveUid() const override;
    [[nodiscard]] QString bootId() const override;
    [[nodiscard]] bool executable(const QString& path) const override;
    [[nodiscard]] QString runtimeDirectory() const override;
    [[nodiscard]] std::optional<uint> directoryOwner(const QString& path) const override;
    [[nodiscard]] bool socket(const QString& path) const override;
    [[nodiscard]] bool file(const QString& path) const override;
    void probeContainment(const QString& program, std::function<void(bool)> done) override;
    void probeMounts(const QString& program, std::function<void(bool)> done) override;

  private:
    QObject m_owner; // of the probes still out
};

} // namespace ketplus::runner::exec
