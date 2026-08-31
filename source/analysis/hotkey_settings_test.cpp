#include "../hotkey_settings.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QFontDatabase>
#include <QtQuick/QQuickItem>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QProcess>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
static bool click(QQuickWindow* window, const char* name) {
    auto* item = window->findChild<QQuickItem*>(QString::fromLatin1(name));
    if (!item) return false;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
    QCoreApplication::processEvents();
    return true;
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--preview")
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    namespace Config = CascadeurHotkeyConfig;
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--check")
        return Config::load(QString::fromLocal8Bit(argv[2])) == QString::fromLocal8Bit(argv[3]).toInt() ? 0 : 1;
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("settings.json");
    CHECK(Config::load(path) == Config::defaultKey);
    QQmlEngine engine;
    QQuickWindow parent;
    parent.resize(800,500); parent.show();
    CHECK(QTest::qWaitForWindowExposed(&parent));
    int saved = Config::defaultKey, closes = 0;
    bool failSave = false;
    auto make = [&]() {
        QString error;
        auto* w = CascadeurHotkeySettings::create(&engine, &parent, Config::load(path),
            [&](int key, QString* why) -> bool {
                if (failSave) { *why = "test failure"; return false; }
                if (!Config::save(path,key,why)) return false;
                saved = key; return true;
            }, [&] { ++closes; }, &error);
        if (!w) std::fprintf(stderr,"%s\n",qPrintable(error));
        else { w->show(); w->requestActivate(); }
        return w;
    };
    QPointer<QQuickWindow> w(make());
    CHECK(w && QTest::qWaitForWindowExposed(w));
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--preview") {
        QTest::qWait(150);
        CHECK(w->grabWindow().save(QString::fromLocal8Bit(argv[2])));
        return 0;
    }
    CHECK(click(w,"hotkeyCapture"));
    QTest::keyClick(w,Qt::Key_A);
    CHECK(w->property("pendingVirtualKey").toInt() == 'A');
    CHECK(!QFile::exists(path));
    CHECK(click(w,"hotkeyAccept") && saved == 'A' && closes == 1);
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    CHECK(!w);
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--check",path,QString::number('A')});
    CHECK(child.waitForFinished(10000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
    w = make(); CHECK(w && QTest::qWaitForWindowExposed(w));
    CHECK(w->property("pendingKeyName").toString() == "A");
    CHECK(click(w,"hotkeyCapture"));
    QTest::keyClick(w,Qt::Key_Escape);
    CHECK(w->isVisible() && !w->property("listening").toBool());
    CHECK(click(w,"hotkeyCapture"));
    QTest::keyClick(w,Qt::Key_F6,Qt::ControlModifier);
    CHECK(w->property("listening").toBool());
    QTest::keyClick(w,Qt::Key_F6);
    CHECK(w->property("pendingVirtualKey").toInt() == 0x75);
    CHECK(click(w,"hotkeyCancel") && Config::load(path) == 'A');
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    w = make(); CHECK(w && QTest::qWaitForWindowExposed(w));
    CHECK(click(w,"hotkeyReset"));
    failSave = true;
    CHECK(click(w,"hotkeyAccept") && w->isVisible() && Config::load(path) == 'A');
    CHECK(!w->property("errorText").toString().isEmpty());
    failSave = false;
    CHECK(click(w,"hotkeyAccept") && Config::load(path) == Config::defaultKey);
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    CHECK(!Config::save(path,0));
    CHECK(!Config::save(dir.path(),Config::defaultKey));
    QString configError;
    for (const QByteArray data : {QByteArray("{"),QByteArray("[]"),QByteArray(4097,' '),
         QByteArray("{\"version\":1.5,\"toggleVirtualKey\":65}"),
         QByteArray("{\"version\":1,\"toggleVirtualKey\":65.5}"),
         QByteArray("{\"version\":1,\"toggleVirtualKey\":0}")}) {
        QFile bad(path); CHECK(bad.open(QIODevice::WriteOnly));
        CHECK(bad.write(data) == data.size()); bad.close();
        CHECK(Config::load(path,&configError) == Config::defaultKey && !configError.isEmpty());
    }
    CHECK(Config::save(path,Config::defaultKey,&configError) && configError.isEmpty());
    w = make(); CHECK(w && QTest::qWaitForWindowExposed(w));
    CHECK(click(w,"hotkeyCapture"));
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(w,&deactivate);
    CHECK(!w->property("listening").toBool());
    w->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    for (int key : {Qt::Key_F1,Qt::Key_F24,Qt::Key_Z,Qt::Key_0,Qt::Key_Space,Qt::Key_Delete})
        CHECK(Config::toQtKey(Config::toVirtualKey(key)) == key);
    std::puts("PASS: recording, Escape, modifier rejection, Cancel, reset, save failure, separate-process persistence");
    std::printf("User config: %s\n",qPrintable(Config::path()));
    return 0;
}
