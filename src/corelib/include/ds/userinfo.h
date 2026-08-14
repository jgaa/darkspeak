#ifndef USERINFO_H
#define USERINFO_H

#include <QString>
#include <QByteArray>
#include <QStringList>

namespace ds {
namespace core {

struct UserInfo {
    QString nickName;
    QByteArray avatarHash;
    bool allowUtf8 = true;
    bool allowFiles = true;
    QStringList allowMimeTypes{QStringLiteral("*")};
    bool allowReactions = true;
    bool publicBio = true;
    bool publicAvatar = true;
};

}} // namespaces

#endif // USERINFO_H
