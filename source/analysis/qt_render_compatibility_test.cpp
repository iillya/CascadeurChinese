// Exercise real exported Qt Quick entry points through Detours. No Cascadeur
// process, scene files, registry or user configuration is touched.
#include "../hook.cpp"
#include <QtGui/QFontDatabase>
#include <QtGui/QImage>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (false)
static AddTextLayoutFn nativeDraw = nullptr;
static QStringList rendered;
static void __fastcall observeDraw(void* node, const QPointF& position, QTextLayout* layout,
    const QColor& color, int style, const QColor& styleColor, const QColor& anchorColor,
    const QColor& selectionColor, const QColor& selectedColor,
    int selectionStart, int selectionEnd, int lineStart, int lineCount) {
    if (layout) rendered.append(layout->text());
    nativeDraw(node, position, layout, color, style, styleColor, anchorColor,
        selectionColor, selectedColor, selectionStart, selectionEnd, lineStart, lineCount);
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    CHECK(argc == 2 && QByteArray(qVersion()) == argv[1]);
    CHECK(CascadeurQtCompatibility::supportedRuntime(qVersion()));
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    CHECK(installHook());
    CHECK(g_textNodeTrackingReady && g_fontAdvance && g_fontAdvanceOpt && g_fontAdvanceF && g_fontAdvanceFOpt);
    nativeDraw = g_originalAddTextLayout;
    g_originalAddTextLayout = observeDraw;
    auto dictionary = std::make_shared<DictionarySnapshot>();
    dictionary->exact["File"] = "文件";
    dictionary->exact["Armature"] = "骨架";
    publishDictionary(dictionary);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick 2.15
Window {
    width: 400; height: 200; visible: true; color: "#303030"
    Text { objectName: "label"; text: "File"; x: 10; y: 10; color: "white"; font.pixelSize: 22 }
    TextInput { objectName: "input"; text: "File"; x: 10; y: 55; color: "white"; font.pixelSize: 22 }
    Item {
        property bool blockSelectionFollowing: false
        property bool blockSelectionExpanding: false
        y: 100; width: 300; height: 50
        Text { objectName: "sceneName"; text: "Armature"; x: 10; color: "white"; font.pixelSize: 22 }
    }
})QML", QUrl("qrc:/compatibility.qml"));
    std::unique_ptr<QObject> object(component.create());
    if (!object) std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    CHECK(window);
    auto paint = [&] {
        QCoreApplication::processEvents();
        return window->grabWindow();
    };
    const auto chinese = paint();
    CHECK(!chinese.isNull());
    CHECK(rendered.contains(QStringLiteral("文件")));
    CHECK(rendered.contains("File") && rendered.contains("Armature") && !rendered.contains(QStringLiteral("骨架")));
    CHECK(window->findChild<QObject*>("label")->property("text").toString() == "File");
    CHECK(window->findChild<QObject*>("input")->property("text").toString() == "File");
    CHECK(window->findChild<QObject*>("sceneName")->property("text").toString() == "Armature");
    rendered.clear();
    toggleTranslation();
    const auto english = paint();
    CHECK(!english.isNull() && chinese != english);
    CHECK(rendered.contains("File") && !rendered.contains(QStringLiteral("文件")));
    rendered.clear();
    toggleTranslation();
    CHECK(!paint().isNull() && rendered.contains(QStringLiteral("文件")));
    object.reset();
    QCoreApplication::processEvents();
    CHECK(g_textNodePreserve.empty());
    g_originalAddTextLayout = nativeDraw;
    std::printf("PASS Qt %s: actual Detours drawing, CN/EN/CN, original properties, protected input/scene names, node cleanup\n", qVersion());
    return 0;
}
