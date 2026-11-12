#pragma once

#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "machine/MachineRichTranscript.h"
#include "machine/MachineWorkspaceView.h"

#include <QStringList>

namespace ketplus::test {

// The rows a remote conversation shows, read from the rich renderer's model (ketplus#598):
// each row's Markdown body, or its command or target for an activity row without one.
inline QStringList transcriptRows(const MachineWorkspaceView& view) {
    QStringList rows;
    AgentMessageModel* model = view.transcript()->view()->messageModel();
    for (int index = 0; model != nullptr && index < model->sourceCount(); ++index) {
        const QVariantMap row = model->source(index);
        for (const char* field : {"body", "command", "target"})
            if (const QString text = row.value(QLatin1String(field)).toString(); !text.isEmpty()) {
                rows.append(text);
                break;
            }
    }
    return rows;
}

} // namespace ketplus::test
