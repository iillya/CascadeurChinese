#include "../scene_text_policy.h"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlComponent>
#include <cstdio>
#include <memory>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    auto check = [&](const char* qml, const char* url, bool expected) {
        QQmlComponent component(&engine);
        component.setData(qml, QUrl(QString::fromLatin1(url)));
        std::unique_ptr<QObject> root(component.create());
        auto* item = qobject_cast<QQuickItem*>(root.get());
        if (!item) { std::fprintf(stderr, "%s\n", qPrintable(component.errorString())); return false; }
        QQuickItem child(item);
        return CascadeurSceneTextPolicy::preserve(item) == expected &&
               CascadeurSceneTextPolicy::preserve(&child) == expected;
    };
    if (!check("import QtQuick; Item { property bool blockSelectionFollowing: false; property bool blockSelectionExpanding: false }", "qrc:/OutlinerTreeView.qml", true)) return 1;
    if (!check("import QtQuick; Item {}", "qrc:/AutoRiggingToolBlank.qml", true)) return 2;
    if (!check("import QtQuick; Item {}", "qrc:/AutoRiggingToolDepth.qml", true)) return 3;
    if (!check("import QtQuick; Item {}", "qrc:/AutoRiggingToolJoints.qml", false)) return 4;
    if (!check("import QtQuick; Item {}", "qrc:/OutlinerTreeView.qml", false)) return 5;
    if (!check("import QtQuick; TextInput { text: 'pelvis' }", "qrc:/Editor.qml", true)) return 6;
    if (!check("import QtQuick; Text { text: 'pelvis' }", "qrc:/PropertyLabel.qml", false)) return 7;
    std::puts("7 scene text policy cases passed (root and child)");
}
