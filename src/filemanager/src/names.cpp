#include <dsfilemanager/filemanager.h>
#include <QSet>

namespace dsfm {
Result<QString> normalizeName(QString name) {
    const auto invalid = Error{ErrorCode::InvalidName};
    if (name.isEmpty() || name.size() > 255 || !name.isValidUtf16()) return invalid;
    for (auto ch : name) {
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format
            || QStringLiteral("/\\<>:\"|?*").contains(ch)) return invalid;
    }
    name = name.normalized(QString::NormalizationForm_C);
    if (name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ')
        || name.size() > 255 || name.toUtf8().size() > 255) return invalid;
    const auto folded = name.toCaseFolded().normalized(QString::NormalizationForm_C);
    const auto base = folded.section('.', 0, 0);
    static const QSet<QString> reserved = {"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"};
    if (folded == ".filemanager" || folded == "objects" || reserved.contains(base)) return invalid;
    if (base.startsWith("com") || base.startsWith("lpt")) {
        const auto digit = base.mid(3);
        if (digit.size() == 1 && (QStringLiteral("123456789¹²³").contains(digit))) return invalid;
    }
    return name;
}
static void fullFold(QString& out, char32_t scalar) {
    switch (scalar) {
#include "fullfold.inc"
    default:
        if (QChar::requiresSurrogates(scalar)) { out.append(QChar(QChar::highSurrogate(scalar))); out.append(QChar(QChar::lowSurrogate(scalar))); }
        else out.append(QChar(char16_t(scalar)));
        return;
    }
}
Result<QString> nameKey(QString name) {
    auto normalized = normalizeName(std::move(name));
    if (!normalized) return normalized.error();
    QString folded;
    folded.reserve(normalized.value().size() * 3);
    for (auto scalar : normalized.value().toUcs4()) fullFold(folded, char32_t(scalar));
    return folded.normalized(QString::NormalizationForm_C);
}
} // namespace dsfm
