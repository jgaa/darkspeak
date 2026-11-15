#pragma once
#include <QByteArray>
#include <sqlite3.h>
namespace dsfm::detail {
inline QByteArray asciiLabel(QByteArrayView bytes) {
    QByteArray output;
    output.reserve(bytes.size() * 3);
    constexpr char hex[] = "0123456789ABCDEF";
    for (char ch : bytes) {
        auto b = static_cast<unsigned char>(ch);
        if (b >= 32 && b < 127 && b != '%')
            output.append(char(b));
        else {
            output.append('%');
            output.append(hex[b >> 4]);
            output.append(hex[b & 15]);
        }
    }
    return output;
}
inline bool installLabelOrder(sqlite3 *db) {
    return sqlite3_create_function_v2(
               db, "dsfm_ascii", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, nullptr,
               [](sqlite3_context *ctx, int, sqlite3_value **values) {
                   auto data = sqlite3_value_text(values[0]);
                   auto size = sqlite3_value_bytes(values[0]);
                   if (!data || size > 2295) {
                       sqlite3_result_error(ctx, "invalid label", -1);
                       return;
                   }
                   auto label = asciiLabel(QByteArrayView(reinterpret_cast<const char *>(data), size));
                   sqlite3_result_text(ctx, label.constData(), label.size(), SQLITE_TRANSIENT);
               },
               nullptr, nullptr, nullptr) == SQLITE_OK;
}
} // namespace dsfm::detail
