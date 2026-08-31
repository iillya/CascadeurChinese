#pragma once
#include <QtQuick/QQuickItem>
#include <QtCore/QMetaObject>
#include <QtQml/QQmlContext>
#include <QtQml/qqml.h>
#include <cstring>

namespace CascadeurSceneTextPolicy {
inline bool qmlType(const char* actual, const char* expected)
{
    const size_t n = std::strlen(expected);
    return std::strncmp(actual, expected, n) == 0 &&
        (actual[n] == '\0' || std::strncmp(actual + n, "_QML_", 5) == 0);
}

// Metadata only: never evaluate QML bindings, inspect model data, or rewrite names.
// Called during scene-graph node creation (GUI/render synchronization), or on GUI.
inline bool preserve(const QQuickItem* item)
{
    for (int depth = 0; item && depth < 128; ++depth, item = item->parentItem()) {
        // A property-free QML root may retain QQuickRowLayout as its class name.
        // Its creation context still identifies the host component file.
        if (const QQmlContext* context = qmlContext(item)) {
            const QString file = context->baseUrl().fileName();
            if (file == QLatin1String("AutoRiggingToolBlank.qml") ||
                file == QLatin1String("AutoRiggingToolDepth.qml")) return true;
        }
        for (const QMetaObject* meta = item->metaObject(); meta; meta = meta->superClass()) {
            const char* name = meta->className();
            if (qmlType(name, "AutoRiggingToolBlank") || qmlType(name, "AutoRiggingToolDepth"))
                return true;
        }
        // These two declared properties identify the Outliner list, not its
        // sibling column header/filter/toolbar (verified in extracted host QML).
        const QMetaObject* meta = item->metaObject();
        if (meta->indexOfProperty("blockSelectionFollowing") >= 0 &&
            meta->indexOfProperty("blockSelectionExpanding") >= 0)
            return true;
        if (item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit"))
            return true;
    }
    return item != nullptr; // excessive ancestry: preserve rather than guess
}
}
