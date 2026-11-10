#include "app/ReviewCapture.h"

#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace ketplus {
namespace {

QString plainText(QString text) { return text.remove(QLatin1Char('&')).trimmed(); }

// "View/Terminal" or "Edit/Settings…": a menu bar heading, then the action's label.
QAction* menuAction(QMainWindow& window, const QString& path) {
    const auto parts = path.split(QLatin1Char('/'));
    QList<QAction*> actions = window.menuBar()->actions();
    for (qsizetype index = 0; index < parts.size(); ++index) {
        QAction* found = nullptr;
        for (auto* action : actions) {
            if (plainText(action->text()) == parts.at(index)) {
                found = action;
                break;
            }
        }
        if (found == nullptr)
            return nullptr;
        if (index + 1 == parts.size())
            return found;
        if (found->menu() == nullptr)
            return nullptr;
        actions = found->menu()->actions();
    }
    return nullptr;
}

QWidget* topWindow(QMainWindow& window) {
    if (auto* modal = QApplication::activeModalWidget())
        return modal;
    return &window;
}

// One step: "Menu/Action" triggers it, "click:Label" presses the button with that text or
// accessible name in the top-most window, "type:Text" enters text in the focused field.
bool runStep(QMainWindow& window, const QString& step) {
    if (step.startsWith(QLatin1String("click:"))) {
        const auto label = step.mid(6);
        for (auto* button : topWindow(window)->findChildren<QAbstractButton*>()) {
            if (button->isVisible() &&
                (plainText(button->text()) == label || button->accessibleName() == label)) {
                button->click();
                return true;
            }
        }
        return false;
    }
    if (step.startsWith(QLatin1String("type:"))) {
        auto* field = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        if (field == nullptr)
            return false;
        field->setText(step.mid(5));
        emit field->returnPressed();
        return true;
    }
    auto* action = menuAction(window, step);
    if (action == nullptr)
        return false;
    // Queued, so a modal dialog's own event loop does not hold the remaining steps.
    QTimer::singleShot(0, action, &QAction::trigger);
    return true;
}

} // namespace

void prepareReviewCapture() {
    // KETPLUS_CAPTURE_ZOOM=90|100|110|125 (ADR 0104).
    if (const auto zoom = qEnvironmentVariable("KETPLUS_CAPTURE_ZOOM"); !zoom.isEmpty())
        QSettings().setValue(QStringLiteral("appearance/interfaceZoomPercent"), zoom.toInt());
}

void runReviewCapture(ThemeManager& theme, QMainWindow& window, const QString& file) {
    // The mock's window size (mockups/qt), so captures compare one to one.
    window.resize(1280, 820);
    const QString captureTheme = qEnvironmentVariable("KETPLUS_CAPTURE_THEME");
    if (captureTheme == QLatin1String("light")) {
        theme.previewMode(ThemeManager::Mode::Light);
    } else if (captureTheme == QLatin1String("dark")) {
        theme.previewMode(ThemeManager::Mode::Dark);
    }

    // KETPLUS_CAPTURE_STEPS: steps separated by ';', run 300 ms apart after the window lays out.
    const auto steps =
        qEnvironmentVariable("KETPLUS_CAPTURE_STEPS").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    const auto failed = std::make_shared<bool>(false);
    int delay = 400;
    for (const auto& step : steps) {
        QTimer::singleShot(delay, &window, [&window, step, failed] {
            if (!runStep(window, step.trimmed())) {
                std::fprintf(stderr, "KETPLUS_CAPTURE step not found: %s\n", qPrintable(step));
                *failed = true;
            }
        });
        delay += 300;
    }
    QTimer::singleShot(delay + 700, &window, [&window, file, failed] {
        const bool saved = !*failed && topWindow(window)->grab().save(file);
        std::fprintf(stderr, "KETPLUS_CAPTURE %s: %s\n", saved ? "saved" : "failed",
                     qPrintable(file));
        QApplication::exit(saved ? 0 : 1);
    });
}

} // namespace ketplus
