#include "teams/TeamCommands.h"

#include "KetButton.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QTimer>

#include <algorithm>

namespace ketplus {
namespace {

// The kit's pressed look on the control that was activated, kept while it is disabled.
constexpr ket::InteractionState kPressed{false, true, true, false};

} // namespace

TeamCommands::TeamCommands(TeamDirectory* teams) : QObject(teams), teams_(teams) {}

QString TeamCommands::key(const QString& teamId, const QString& what) {
    return teamId + QLatin1Char('/') + what;
}

bool TeamCommands::isWaitingLong(const QString& key) const {
    const auto it = pending_.constFind(key);
    return it != pending_.cend() && it->waitingLong;
}

bool TeamCommands::send(const QString& key, const QString& source, const QString& type,
                        const QJsonObject& payload, Reply reply) {
    if (pending_.contains(key))
        return false;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, key] {
        const auto it = pending_.find(key);
        if (it == pending_.end())
            return;
        it->waitingLong = true;
        applyAll(key);
        emit pendingChanged(key);
    });
    pending_.insert(key, Pending{source, false, timer});
    prune();
    for (const Bound& bound : bound_)
        if (auto* label = qobject_cast<QLabel*>(bound.control.data());
            bound.status && bound.key == key && label) {
            label->clear();
            label->setVisible(false);
        }
    timer->start(stillWaiting_);
    applyAll(key);
    emit pendingChanged(key);
    const QPointer<TeamCommands> alive(this);
    // The directory stores a team in the answer before this runs, so the page has already
    // drawn the confirmed state when the pending one ends.
    teams_->request(
        type, payload,
        [alive, key, reply = std::move(reply)](const QJsonObject& result, const QString& error) {
            if (alive)
                alive->finish(key, error);
            if (reply)
                reply(result, error);
        });
    return true;
}

void TeamCommands::bind(QAbstractButton* control, const QString& key, const QString& source) {
    prune();
    bound_.append(Bound{control, key, source});
    if (pending_.contains(key))
        apply(bound_.last());
}

void TeamCommands::bind(QAction* action, const QString& key) {
    prune();
    bound_.append(Bound{action, key});
    if (pending_.contains(key))
        apply(bound_.last());
}

void TeamCommands::bindStatus(QLabel* label, const QString& key) {
    prune();
    Bound status{label, key};
    status.status = true;
    bound_.append(status);
    if (pending_.contains(key))
        apply(bound_.last());
}

void TeamCommands::finish(const QString& key, const QString& error) {
    const auto it = pending_.find(key);
    if (it == pending_.end())
        return;
    it->timer->deleteLater();
    pending_.erase(it);
    prune();
    for (Bound& bound : bound_) {
        if (bound.key != key)
            continue;
        restore(bound);
        if (auto* label = qobject_cast<QLabel*>(bound.control.data());
            bound.status && label && !error.isEmpty()) {
            label->setText(error);
            label->setVisible(true);
        }
    }
    emit pendingChanged(key);
}

void TeamCommands::applyAll(const QString& key) {
    prune();
    for (Bound& bound : bound_)
        if (bound.key == key)
            apply(bound);
    // Disabling one control after another moves the focus along them: where it rests once
    // all are disabled is where a refusal takes it back from.
    QWidget* rest = QApplication::focusWidget();
    for (Bound& bound : bound_)
        if (bound.key == key && bound.hadFocus)
            bound.focusMovedTo = rest;
}

bool TeamCommands::activated(const Bound& bound) const {
    const auto it = pending_.constFind(bound.key);
    return it != pending_.cend() && it->source == bound.source;
}

bool TeamCommands::hasStatus(const QString& key) const {
    return std::any_of(bound_.cbegin(), bound_.cend(), [&](const Bound& bound) {
        return bound.status && bound.key == key && bound.control;
    });
}

void TeamCommands::prune() {
    bound_.removeIf([](const Bound& bound) { return !bound.control; });
}

void TeamCommands::apply(Bound& bound) {
    const bool waiting = isWaitingLong(bound.key);
    const QString stillWaiting = tr("Still waiting…");
    if (bound.status) {
        auto* label = qobject_cast<QLabel*>(bound.control.data());
        if (!label || !waiting || bound.waitingShown)
            return;
        bound.waitingShown = true;
        label->setText(tr("Still waiting for KetPlus Daemon. The command stays pending until it "
                          "answers."));
        if (auto* text = qobject_cast<team_page::Text*>(label))
            text->setTone(QStringLiteral("text-secondary"));
        label->setVisible(true);
        return;
    }
    if (auto* action = qobject_cast<QAction*>(bound.control.data())) {
        if (!bound.applied) {
            bound.applied = true;
            bound.enabled = action->isEnabled();
            bound.text = action->text();
            action->setEnabled(false);
        }
        if (waiting && !bound.waitingShown) {
            bound.waitingShown = true;
            action->setText(tr("%1 (still waiting)").arg(bound.text));
        }
        return;
    }
    auto* control = qobject_cast<QAbstractButton*>(bound.control.data());
    if (!control)
        return;
    const bool mine = activated(bound);
    if (!bound.applied) {
        bound.applied = true;
        bound.enabled = control->isEnabled();
        bound.text = control->text();
        bound.hadFocus = control->hasFocus();
        control->setEnabled(false);
        if (bound.hadFocus)
            bound.focusMovedTo = QApplication::focusWidget();
        if (mine) {
            if (auto* button = qobject_cast<ket::Button*>(control))
                button->setPreviewState(kPressed);
            control->setProperty("teamCommandPending", true);
            control->setAccessibleDescription(tr("Sending…"));
            control->update();
        }
    }
    if (mine && waiting && !bound.waitingShown) {
        bound.waitingShown = true;
        control->setToolTip(stillWaiting);
        control->setAccessibleDescription(stillWaiting);
        // A label beside the controls says it in full; without one the control says it.
        if (!hasStatus(bound.key) && qobject_cast<ket::Button*>(control))
            control->setText(stillWaiting);
    }
}

void TeamCommands::restore(Bound& bound) {
    const bool waitingShown = bound.waitingShown;
    bound.waitingShown = false;
    if (bound.status) {
        auto* label = qobject_cast<QLabel*>(bound.control.data());
        if (!label || !waitingShown)
            return;
        label->clear();
        label->setVisible(false);
        if (auto* text = qobject_cast<team_page::Text*>(label))
            text->setTone(QStringLiteral("danger"));
        return;
    }
    if (!bound.applied)
        return;
    bound.applied = false;
    if (auto* action = qobject_cast<QAction*>(bound.control.data())) {
        action->setText(bound.text);
        action->setEnabled(bound.enabled);
        return;
    }
    auto* control = qobject_cast<QAbstractButton*>(bound.control.data());
    if (!control)
        return;
    if (control->property("teamCommandPending").toBool()) {
        if (auto* button = qobject_cast<ket::Button*>(control))
            button->setPreviewState(std::nullopt);
        control->setProperty("teamCommandPending", false);
        control->setAccessibleDescription({});
        control->setToolTip({});
    }
    if (waitingShown && control->text() != bound.text)
        control->setText(bound.text);
    control->setEnabled(bound.enabled);
    control->update();
    // The focus goes back to the control when nothing else took it meanwhile.
    const QWidget* now = QApplication::focusWidget();
    if (bound.hadFocus && bound.enabled && (now == nullptr || now == bound.focusMovedTo))
        control->setFocus(Qt::OtherFocusReason);
    bound.hadFocus = false;
    bound.focusMovedTo = nullptr;
}

} // namespace ketplus
