// Exercise production functions without attaching Detours or starting Cascadeur.
#include "../hook.cpp"
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <cstdio>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(false)
static QString drawn;
static int drawnLines = 0;
static void __fastcall recordLayout(void*, const QPointF&, QTextLayout* layout, const QColor&, int,
    const QColor&,const QColor&,const QColor&,const QColor&,int,int,int,int lines) {
    drawn = layout ? layout->text() : QString(); drawnLines = lines;
}
int main(int argc, char** argv) {
    QGuiApplication app(argc,argv);
    {
        DisplayHookGuard outer;
        { DisplayHookGuard inner; CHECK(g_inDisplayHook); }
        CHECK(g_inDisplayHook);
    }
    CHECK(!g_inDisplayHook);
    auto dictionary = std::make_shared<DictionarySnapshot>();
    dictionary->exact["File"] = "文件";
    dictionary->exact["Synchronization"] = "同步";
    dictionary->folded["file"] = "文件";
    publishDictionary(dictionary);
    CHECK(translateText("File") == QStringLiteral("文件"));
    CHECK(translateText("FILE") == QStringLiteral("文件"));
    CHECK(translateText("C:\\File").isEmpty());
    const QString sample = QStringLiteral("File");
    QElapsedTimer timer; timer.start();
    for (int i=0;i<200000;++i) if (translateText(sample).isEmpty()) return 2;
    std::printf("BENCH exact lookup 200000: %lld ms\n",timer.elapsed());
    auto next = std::make_shared<DictionarySnapshot>(*dictionary);
    next->exact["File"] = "文件2";
    publishDictionary(next);
    CHECK(translateText("File") == QStringLiteral("文件2"));
    publishDictionary(dictionary);
    bool threadOk = false;
    std::thread reader([&] { threadOk = translateText("File") == QStringLiteral("文件"); });
    reader.join(); CHECK(threadOk);
    QTextLayout source(QStringLiteral("File"),QFont());
    source.beginLayout(); source.createLine().setLineWidth(400); source.endLayout();
    int nodeToken = 0;
    g_originalAddTextLayout = recordLayout;
    g_textNodeTrackingReady.store(true);
    g_textNodePreserve[&nodeToken] = false;
    const QColor color(Qt::white);
    auto draw = [&](int selection=-1,int count=1) {
        hookedAddTextLayout(&nodeToken,{},&source,color,0,color,color,color,color,selection,-1,0,count);
    };
    draw(); CHECK(drawn == QStringLiteral("文件") && source.text() == "File" && drawnLines == 1);
    draw(-1,-1); CHECK(drawn == QStringLiteral("文件") && drawnLines == -1);
    draw(0); CHECK(drawn == "File");
    draw(-1,0); CHECK(drawn == "File");
    g_textNodePreserve[&nodeToken] = true; draw(); CHECK(drawn == "File");
    g_textNodePreserve.erase(&nodeToken); draw(); CHECK(drawn == "File");
    g_enabled.store(false);
    // Holding this mutex proves disabled drawing does not touch the scene map.
    {
        std::lock_guard<std::mutex> lock(g_textNodeMutex);
        draw(); CHECK(drawn == "File");
    }
    timer.restart();
    for (int i=0;i<200000;++i) draw();
    std::printf("BENCH disabled draw 200000: %lld ms\n",timer.elapsed());
    CHECK(translateText("File").isEmpty());
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick 2.15\nimport QtQuick.Controls 2.15\nMenuBar { MenuBarItem { text: 'Synchronization'; width: 30; implicitWidth: 30 } }",QUrl());
    std::unique_ptr<QObject> object(component.create());
    auto* bar = qobject_cast<QQuickItem*>(object.get()); CHECK(bar);
    adjustMenuWidths(bar);
    QQuickItem* item = nullptr;
    CHECK(QMetaObject::invokeMethod(bar,"itemAt",Qt::DirectConnection,Q_RETURN_ARG(QQuickItem*,item),Q_ARG(int,0)));
    CHECK(item && item->property("text").toString() == "Synchronization");
    const QFontMetricsF metrics(item->property("font").value<QFont>());
    CHECK(item->width() >= metrics.horizontalAdvance("Synchronization"));
    const qreal english = item->width();
    g_enabled.store(true); adjustMenuWidths(bar);
    CHECK(item->width() < english && item->property("text").toString() == "Synchronization");
    g_enabled.store(false); adjustMenuWidths(bar); CHECK(item->width() >= english);
    NativeKeyFilter native;
    native.toggleHandled_ = VK_F3;
    MSG message{}; message.message = WM_KEYUP; message.wParam = VK_F3;
    g_settingsOpen = true;
    CHECK(!native.nativeEventFilter("windows_generic_MSG",&message,nullptr) && !native.toggleHandled_);
    g_settingsOpen = false;
    auto cpuTicks = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return uint64_t(0);
        return (uint64_t(kernel.dwHighDateTime)<<32) + kernel.dwLowDateTime +
               (uint64_t(user.dwHighDateTime)<<32) + user.dwLowDateTime;
    };
    auto idle = [&] {
        const uint64_t before = cpuTicks();
        QEventLoop loop;
        QTimer::singleShot(1000,&loop,&QEventLoop::quit);
        loop.exec();
        return double(cpuTicks()-before)/10000.0;
    };
    const double baselineCpu = idle();
    installLifecycleFilter(&app);
    const double installedCpu = idle();
    std::printf("IDLE test-process CPU per 1s: baseline %.3f ms, lifecycle installed %.3f ms (not host FPS/CPU)\n",baselineCpu,installedCpu);
    std::puts("PASS: layout range/scene exclusions, dictionary publication, disabled fast path, English menu width");
    return 0;
}
