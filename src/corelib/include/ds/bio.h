#ifndef DS_BIO_H
#define DS_BIO_H

#include <QRegularExpression>
#include <QString>
#include <utility>

namespace ds::core::bio {

inline constexpr qsizetype maxCharacters = 1000;
inline constexpr qsizetype maxUtf8Bytes = 4096;

// Keep Markdown text, but make raw HTML and media syntax inert. Image/media
// syntax is retained as escaped text so the author can see what was removed.
inline QString sanitize(QString value)
{
    value = value.left(maxCharacters);
    value.replace(QRegularExpression(QStringLiteral(R"(<)")), QStringLiteral("&lt;"));
    value.replace(QRegularExpression(QStringLiteral(R"(>)")), QStringLiteral("&gt;"));

    static const QRegularExpression media{
        QStringLiteral(R"(!\[([^\]\r\n]{0,256})\]\(([^)\r\n]{1,2048})\))")};
    QString rewritten;
    rewritten.reserve(value.size());
    qsizetype cursor = 0;
    auto iterator = media.globalMatch(value);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        rewritten += value.mid(cursor, match.capturedStart() - cursor);
        rewritten += QStringLiteral(R"(\![)" );
        rewritten += match.captured(1);
        rewritten += QStringLiteral(R"(]\()" );
        rewritten += match.captured(2);
        rewritten += QStringLiteral(R"(\))" );
        cursor = match.capturedEnd();
    }
    rewritten += value.mid(cursor);
    value = std::move(rewritten);
    return value;
}

inline bool withinLimits(const QString& value)
{
    return value.size() <= maxCharacters && value.toUtf8().size() <= maxUtf8Bytes;
}

}

#endif
