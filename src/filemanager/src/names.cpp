#include <dsfilemanager/filemanager.h>
#include <QSet>

namespace dsfm {
Result<QString> encodeLinuxName(QByteArrayView name) {
    if (name.isEmpty() || name.size() > max_linux_name_bytes || name == "." || name == "..")
        return Error{ErrorCode::InvalidName};
    // Validate the whole component before producing an encoded result.
    for (const auto byte : name) if (byte == '\0' || byte == '/') return Error{ErrorCode::InvalidName};
    QString result;
    result.reserve(name.size() * 3);
    constexpr char hex[] = "0123456789ABCDEF";
    const auto escape = [&](qsizetype offset, qsizetype length) {
        for (qsizetype i = offset; i < offset + length; ++i) {
            const auto byte = static_cast<unsigned char>(name[i]);
            result.append('%'); result.append(QLatin1Char(hex[byte >> 4])); result.append(QLatin1Char(hex[byte & 15]));
        }
    };
    for (qsizetype offset = 0; offset < name.size();) {
        const auto first = static_cast<unsigned char>(name[offset]);
        qsizetype length = 1;
        char32_t scalar = first;
        char32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { length = 2; scalar = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { length = 3; scalar = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 4; scalar = first & 0x07; minimum = 0x10000; }
        bool valid = first < 0x80 || length > 1;
        if (length > name.size() - offset) valid = false;
        if (valid && length > 1) {
            for (qsizetype i = 1; i < length; ++i) {
                const auto byte = static_cast<unsigned char>(name[offset + i]);
                if ((byte & 0xc0) != 0x80) { valid = false; break; }
                scalar = (scalar << 6) | (byte & 0x3f);
            }
            valid = valid && scalar >= minimum && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff);
        }
        if (!valid) { escape(offset, 1); ++offset; continue; }
        const auto category = QChar::category(scalar);
        const bool unsafe = scalar == '%' || category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph
            || (scalar >= 0xfdd0 && scalar <= 0xfdef) || (scalar & 0xffff) >= 0xfffe;
        if (unsafe) escape(offset, length);
        else result.append(QString::fromUtf8(name.data() + offset, length));
        offset += length;
    }
    return result;
}
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
