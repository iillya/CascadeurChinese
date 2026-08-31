#pragma once

#include <QtCore/QHash>
#include <QtCore/QString>

namespace CascadeurTranslationPolicy {
inline bool containsCjk(const QString& text)
{
    for (QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x3400 && u <= 0x9fff) || (u >= 0xf900 && u <= 0xfaff)) return true;
    }
    return false;
}

inline QString normalizeUncached(QString text)
{
    text.replace(QChar(0x00a0), QLatin1Char(' '));
    text.replace(QChar(0x3000), QLatin1Char(' '));
    text.remove(QChar(0x00ad)); text.remove(QChar(0x200b));
    text.remove(QChar(0x200c)); text.remove(QChar(0x200d)); text.remove(QChar(0xfeff));
    for (QChar& ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0xff01 && u <= 0xff5e) ch = QChar(u - 0xfee0);
    }
    text.replace(QChar(0x2018), QLatin1Char('\''));
    text.replace(QChar(0x2019), QLatin1Char('\''));
    text.replace(QChar(0x201c), QLatin1Char('"'));
    text.replace(QChar(0x201d), QLatin1Char('"'));
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text = text.simplified();
    while (text.endsWith(QChar(0x2026))) text.chop(1);
    while (text.endsWith(QLatin1String("..."))) text.chop(3);
    if (text.endsWith(QLatin1String(" *"))) text.chop(2);
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.remove(QLatin1Char('&'));
    text = text.normalized(QString::NormalizationForm_D);
    QString withoutMarks;
    withoutMarks.reserve(text.size());
    for (QChar ch : text)
        if (ch.category() != QChar::Mark_NonSpacing &&
            ch.category() != QChar::Mark_SpacingCombining &&
            ch.category() != QChar::Mark_Enclosing)
            withoutMarks.append(ch);
    text = withoutMarks.normalized(QString::NormalizationForm_C).toCaseFolded();
    text = text.simplified();
    return text;
}

inline bool looksTranslatable(const QString& source)
{
    const QString text = source.trimmed();
    if (text.size() < 2 || text.size() > 500 || containsCjk(text)) return false;
    bool letter = false;
    for (QChar ch : text) if (ch.isLetter()) { letter = true; break; }
    if (!letter || text.contains(QLatin1String(":\\")) || text.contains(QLatin1String(":/")) ||
        text.contains(QLatin1Char('\\')) || text.startsWith(QLatin1Char('/')) ||
        text.startsWith(QLatin1String("./")) || text.startsWith(QLatin1String("../")) ||
        (text.startsWith(QLatin1Char('<')) && text.endsWith(QLatin1Char('>'))) ||
        text.contains(QLatin1String("</")) || text.contains(QLatin1String("${")) ||
        text.contains(QLatin1String("{{"))) return false;
    if (text.startsWith(QLatin1String("http"), Qt::CaseInsensitive) ||
        text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return false;
    return true;
}

inline QString normalize(const QString& text) {
    // Normalization alone is cached; dictionary reloads cannot stale it.
    thread_local QHash<QString, QString> cache;
    const auto found = cache.constFind(text);
    if (found != cache.cend()) return found.value();
    const QString key = normalizeUncached(text);
    if (text.size() <= 500) {
        if (cache.size() >= 1024) cache.clear();
        cache.insert(text, key);
    }
    return key;
}
} // namespace CascadeurTranslationPolicy
