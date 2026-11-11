#pragma once

#include <QString>
#include <QVariantList>

// The labels a conversation can carry and the colour each one is drawn in. The daemon stores
// which labels a conversation has (`session.tags/1`); names and colours belong to the client.
namespace ketplus::conversation_tags {

// Every label on offer, each as {label, color}.
[[nodiscard]] QVariantList catalog();
// The colour of a label; a label this build does not know gets the neutral one.
[[nodiscard]] QString color(const QString& label);

} // namespace ketplus::conversation_tags
