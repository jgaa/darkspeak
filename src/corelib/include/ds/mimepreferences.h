#pragma once
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <optional>

namespace ds::core {
inline bool validIdentityMimePreferences(const QStringList &types)
{
    if (types.isEmpty() || types.size() > 64) return false;
    if (types == QStringList{QStringLiteral("*")}) return true;
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9!#$&^_.+\\-]+/[a-z0-9!#$&^_.+\\-]+$"));
    QSet<QString> seen;
    for (const auto &type : types) {
        if (type.size() > 127 || !pattern.match(type).hasMatch() || seen.contains(type)) return false;
        seen.insert(type);
    }
    return true;
}
inline std::optional<QStringList> parseMimePreferences(const QString &text)
{
    if (text.size() > 8192) return std::nullopt;
    QStringList types;
    for (const auto &line : text.split(QChar('\n'))) {
        const auto type = line.trimmed();
        if (!type.isEmpty()) types.push_back(type);
        if (types.size() > 64) return std::nullopt;
    }
    if (!validIdentityMimePreferences(types)) return std::nullopt;
    return types;
}
}
