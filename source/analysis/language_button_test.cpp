#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QQuickItem>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlComponent>
#include <QtCore/QFile>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <memory>
#include <cstdio>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) return 1;
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const QByteArray code = file.readAll();
    const auto function = code.indexOf("QObject* createLanguageMenuItem(");
    const auto begin = code.indexOf("R\"QML(", function);
    const auto end = code.indexOf(")QML\"", begin);
    if (function < 0 || begin < 0 || end < 0) return 3;
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(code.mid(begin + 6, end - begin - 6), QUrl("qrc:/language-test.qml"));
    std::unique_ptr<QObject> object(component.create());
    auto* item = qobject_cast<QQuickItem*>(object.get());
    if (!item) { std::fprintf(stderr, "%s\n", qPrintable(component.errorString())); return 4; }
    QQuickWindow window;
    window.resize(200, 80);
    item->setParentItem(window.contentItem());
    item->setWidth(180);
    item->setHeight(60);
    QSignalSpy settings(item, SIGNAL(hotkeySettingsRequested()));
    QSignalSpy clicked(item, SIGNAL(clicked()));
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) return 5;
    window.requestActivate();
    if (!QTest::qWaitForWindowActive(&window)) return 8;
    QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, QPoint(60, 25));
    QCoreApplication::processEvents();
    if (settings.count() == 0) settings.wait(1000);
    if (settings.count() != 1 || clicked.count() != 0) {
        std::fprintf(stderr, "right click: settings=%lld clicked=%lld Qt=%s\n",
            static_cast<long long>(settings.count()), static_cast<long long>(clicked.count()), qVersion());
        return 6;
    }
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(60, 25));
    QCoreApplication::processEvents();
    if (settings.count() != 1 || clicked.count() != 1) return 7;
    item->setParentItem(nullptr);
    std::puts("PASS: right click requests settings once; left click toggles once");
    return 0;
}
