#include "../numeric_templates.h"
#include <cstdio>

int main()
{
    using namespace NumericTemplates;
    Rule rule;
    if (!compile("Number of selected objects: %1", QString::fromUtf8("选定对象数量：%1"), rule)) return 1;
    QVector<Rule> rules{rule};
    if (lookup(rules, "Number of selected objects: 157") != QString::fromUtf8("选定对象数量：157")) return 2;
    if (lookup(rules, "Number of selected objects: -12.50") != QString::fromUtf8("选定对象数量：-12.50")) return 3;
    for (const auto* invalid : {"Number of selected objects: pelvis", "Number of selected objects: 1,000",
         "Number of selected objects: 1e3", "Number of selected objects: 5...", "Number of selected objects: 5\n",
         "Number of selected objects: ", "number of selected objects: 2", "Number of selected objects: 12345678901234567"})
        if (!lookup(rules, QString::fromLatin1(invalid)).isEmpty()) return 4;
    Rule multi;
    if (!compile("Range %1 to %2, again %1", "%2 / %1 / %1", multi)) return 5;
    if (lookup({multi}, "Range 01 to 20, again 01") != "20 / 01 / 01") return 6;
    if (!lookup({multi}, "Range 01 to 20, again 1").isEmpty()) return 7;
    if (compile("Range %1", "%2", multi) || compile("%1", "%1", multi) ||
        compile("Range %L1", "%L1", multi) || compile("Range %1%2", "%1%2", multi) ||
        compile("Range %1", "%1 %1", multi)) return 8;
    // Removing tokens with QString::remove used to turn the illegal %10 into
    // a literal zero after finding a separate valid %1.
    if (compile("Range %1", "%1 %10", multi) ||
        compile("Range %1", "%1 %1x %10", multi) ||
        compile("Range %1", "%1 %%", multi)) return 10;
    rules.append(rule);
    if (!lookup(rules, "Number of selected objects: 2").isEmpty()) return 9;
    Rule frames;
    if (!compile("Collision cleaning completed from %1 to %2 frames",
        QStringLiteral("已完成第 %1 至第 %2 帧的碰撞清理"), frames)) return 11;
    if (lookup({frames}, "Collision cleaning completed from 12 to 240 frames") !=
        QStringLiteral("已完成第 12 至第 240 帧的碰撞清理")) return 12;
    if (lookup({frames}, "Collision cleaning completed from -12 to 0 frames") !=
        QStringLiteral("已完成第 -12 至第 0 帧的碰撞清理")) return 13;
    if (!lookup({frames}, "Collision cleaning completed from pelvis to 240 frames").isEmpty()) return 14;
    std::puts("numeric template tests passed");
    return 0;
}
