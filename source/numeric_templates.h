#pragma once
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <algorithm>

namespace NumericTemplates {
struct Rule {
    QString source, target;
    QRegularExpression expression;
};

inline QStringList tokens(const QString& text)
{
    QStringList result;
    static const QRegularExpression token(QStringLiteral("%[1-9](?![0-9])"));
    auto matches = token.globalMatch(text);
    while (matches.hasNext()) result.append(matches.next().captured());
    std::sort(result.begin(), result.end());
    return result;
}

inline bool compile(const QString& source, const QString& target, Rule& rule)
{
    const auto ids = tokens(source);
    if (source.size() > 500 || target.size() > 1000 || ids.isEmpty() || ids.size() > 8 ||
        ids != tokens(target)) return false;
    const qsizetype first = source.indexOf(QLatin1Char('%'));
    if (first < 4) return false; // explicit literal prefix, never a catch-all
    QString pattern = QStringLiteral("\\A");
    QHash<QChar, bool> seen;
    for (qsizetype i = 0; i < source.size(); ++i) {
        if (source[i] != QLatin1Char('%')) {
            pattern += QRegularExpression::escape(source.mid(i, 1));
            continue;
        }
        if (i + 1 >= source.size() || source[i+1] < QLatin1Char('1') || source[i+1] > QLatin1Char('9') ||
            (i + 2 < source.size() && (source[i+2].isDigit() || source[i+2] == QLatin1Char('%')))) return false;
        const QChar id = source[++i];
        if (seen.contains(id)) pattern += QStringLiteral("\\k<p%1>").arg(id);
        else {
            // ASCII only; bounded signed integer/decimal. No locale grouping/exponent.
            pattern += QStringLiteral("(?<p%1>[+-]?[0-9]{1,16}(?:\\.[0-9]{1,8})?)").arg(id);
            seen.insert(id, true);
        }
    }
    // Reject unsupported placeholders in the target as well.
    for (qsizetype i = 0; i < target.size(); ++i) {
        if (target[i] != QLatin1Char('%')) continue;
        if (i + 1 >= target.size() || target[i+1] < QLatin1Char('1') ||
            target[i+1] > QLatin1Char('9') ||
            (i + 2 < target.size() && target[i+2].isDigit())) return false;
        ++i;
    }
    QRegularExpression expression(pattern + QStringLiteral("\\z"));
    if (!expression.isValid()) return false;
    rule = {source, target, expression};
    return true;
}

inline QString lookup(const QVector<Rule>& rules, const QString& source)
{
    if (source.size() > 500) return {};
    QString result;
    bool matched = false;
    for (const auto& rule : rules) {
        const auto match = rule.expression.match(source);
        if (!match.hasMatch()) continue;
        // More than one matching rule is ambiguous, even if outputs coincide.
        if (matched) return {};
        matched = true;
        for (qsizetype i = 0; i < rule.target.size(); ++i) {
            if (rule.target[i] == QLatin1Char('%')) {
                result += match.captured(QStringLiteral("p%1").arg(rule.target[++i]));
            } else result += rule.target[i];
        }
    }
    return result;
}
}
