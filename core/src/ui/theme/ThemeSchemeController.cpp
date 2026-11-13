#include "ui/theme/ThemeSchemeController.h"
#include "KetAppearance.h"
#include "ui/theme/ColorSchemeCatalog.h"
#include "ui/theme/ColorSchemeKitAdapter.h"
#include <QApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QtConcurrentRun>

namespace ketplus {
namespace {
bool systemDark() { return qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark; }
} // namespace
ThemeSchemeController::Prepared ThemeSchemeController::prepare(ColorScheme scheme) {
    scheme = resolveColorScheme(scheme);
    auto palette = colorSchemePalette(scheme);
    auto overrides = colorSchemeKitOverrides(scheme);
    return {std::move(scheme), std::move(palette), std::move(overrides)};
}
ThemeSchemeController::ThemeSchemeController(QString directory, QString legacyTheme,
                                             QObject* parent)
    : QObject(parent), store_(new ThemeSelectionStore(directory, legacyTheme, this)),
      directory_(QFileInfo(store_->filePath()).absolutePath()),
      prepared_(prepare(builtinColorScheme(systemDark() ? SchemeKind::Dark : SchemeKind::Light))) {
    committedDark_ = selection().schemeId == u"builtin-dark";
    connect(store_, &ThemeSelectionStore::changed, this, [this] {
        previewSelection_.reset();
        previewScheme_.reset();
        request();
    });
    connect(store_, &ThemeSelectionStore::failed, this, &ThemeSchemeController::failed);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive)
                    store_->refresh();
            });
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (!useKitSystem() && !previewScheme_ &&
            previewSelection_.value_or(selection()).followSystem)
            request();
    });
    // Custom preferences pin the kit to a complete snapshot; the standard
    // built-in pair retains the legacy Appearance System mode below.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this, [this] {
        if (publishing_) {
            emit applied();
            return;
        }
        auto& appearance = ket::Appearance::instance();
        if (useKitSystem() && appearance.mode() == ket::Appearance::Mode::System) {
            // Preserve the existing Appearance System contract for the standard
            // built-in pair. Both branches were cleared before entering it.
            prepared_ = prepare(
                builtinColorScheme(appearance.dark() ? SchemeKind::Dark : SchemeKind::Light));
            emit applied();
            return;
        }
        // Existing integrations may still set Appearance directly. Treat that as
        // a local built-in preview, without publishing a selection-file revision.
        auto wanted = selection();
        wanted.followSystem = appearance.mode() == ket::Appearance::Mode::System;
        wanted.schemeId =
            appearance.dark() ? QStringLiteral("builtin-dark") : QStringLiteral("builtin-light");
        previewSelection_ = wanted;
        previewScheme_.reset();
        request();
    });
}
void ThemeSchemeController::start() { request(); }
const ColorScheme& ThemeSchemeController::scheme() const { return prepared_.scheme; }
const ThemePalette& ThemeSchemeController::palette() const { return prepared_.palette; }
const ThemeSelection& ThemeSchemeController::selection() const { return store_->selection(); }
ThemeManager::Mode ThemeSchemeController::mode() const {
    return selection().followSystem ? ThemeManager::Mode::System
           : committedDark_         ? ThemeManager::Mode::Dark
                                    : ThemeManager::Mode::Light;
}
bool ThemeSchemeController::hasPreview() const { return previewSelection_ || previewScheme_; }
QString ThemeSchemeController::lastError() const { return store_->lastError(); }
bool ThemeSchemeController::commit(const ThemeSelection& wanted) {
    const auto revision = store_->revision();
    if (!store_->commit(wanted))
        return false;
    if (revision == store_->revision() && hasPreview()) {
        previewSelection_.reset();
        previewScheme_.reset();
        request();
    }
    return true;
}
bool ThemeSchemeController::commitPreview() {
    // An in-memory import must first be installed in the catalog; this API does
    // not install untrusted/workspace data or silently persist a preview.
    if (previewScheme_)
        return false;
    return previewSelection_ ? commit(*previewSelection_) : true;
}
void ThemeSchemeController::preview(const ThemeSelection& wanted) {
    if (previewSelection_ == wanted && !previewScheme_)
        return;
    previewSelection_ = wanted;
    previewScheme_.reset();
    request();
}
void ThemeSchemeController::preview(const ColorScheme& scheme) {
    previewSelection_.reset();
    previewScheme_ = scheme;
    request();
}
void ThemeSchemeController::cancelPreview() {
    if (!hasPreview())
        return;
    previewSelection_.reset();
    previewScheme_.reset();
    request();
}
bool ThemeSchemeController::useKitSystem() const {
    const auto wanted = previewSelection_.value_or(selection());
    return !previewScheme_ && wanted.followSystem && wanted.lightSchemeId == u"builtin-light" &&
           wanted.darkSchemeId == u"builtin-dark";
}
void ThemeSchemeController::request() {
    const auto generation = ++generation_;
    const auto wanted = previewSelection_.value_or(selection());
    const auto fallback =
        wanted.followSystem && systemDark() ? SchemeKind::Dark : SchemeKind::Light;
    const auto id = wanted.followSystem
                        ? (systemDark() ? wanted.darkSchemeId : wanted.lightSchemeId)
                        : wanted.schemeId;
    if (previewScheme_) {
        auto* watcher = new QFutureWatcher<Prepared>(this);
        connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, watcher, generation] {
            auto prepared = watcher->result();
            watcher->deleteLater();
            publish(std::move(prepared), generation);
        });
        watcher->setFuture(
            QtConcurrent::run([scheme = *previewScheme_] { return prepare(scheme); }));
        return;
    }
    // Embedded snapshots are tiny and cached by the model. Built-in mode changes
    // stay synchronous for existing callers; user-file parsing stays off the GUI.
    if (id == u"builtin-light" || id == u"builtin-dark") {
        publish(prepare(builtinColorScheme(id == u"builtin-dark" ? SchemeKind::Dark
                                                                 : SchemeKind::Light)),
                generation);
        return;
    }
    auto* watcher = new QFutureWatcher<Prepared>(this);
    connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, watcher, generation] {
        auto prepared = watcher->result();
        watcher->deleteLater();
        publish(std::move(prepared), generation);
    });
    watcher->setFuture(QtConcurrent::run([directory = directory_, id, fallback] {
        return prepare(ColorSchemeCatalog(directory).load(id, fallback).scheme);
    }));
}
void ThemeSchemeController::publish(Prepared prepared, quint64 generation) {
    if (generation != generation_)
        return;
    prepared_ = std::move(prepared);
    if (!hasPreview())
        committedDark_ = prepared_.palette.dark;
    auto& appearance = ket::Appearance::instance();
    const auto branch = prepared_.palette.dark ? ket::Theme::Dark : ket::Theme::Light;
    {
        const QSignalBlocker blocker(appearance);
        if (useKitSystem()) {
            appearance.clearColorOverrides(branch == ket::Theme::Dark ? ket::Theme::Light
                                                                      : ket::Theme::Dark);
            appearance.setMode(ket::Appearance::Mode::System);
        } else {
            appearance.setMode(prepared_.palette.dark ? ket::Appearance::Mode::Dark
                                                      : ket::Appearance::Mode::Light);
        }
    }
    publishing_ = true;
    const auto rejected = appearance.setColorOverrides(branch, prepared_.overrides);
    Q_ASSERT(rejected.isEmpty());
    publishing_ = false;
}
} // namespace ketplus
