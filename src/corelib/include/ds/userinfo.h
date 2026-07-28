#ifndef USERINFO_H
#define USERINFO_H

#include <QString>
#include <QByteArray>

namespace ds {
namespace core {

struct UserInfo {
    QString nickName;
    QByteArray avatarHash;
};

}} // namespaces

#endif // USERINFO_H
