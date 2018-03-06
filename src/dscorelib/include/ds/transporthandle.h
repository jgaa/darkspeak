#ifndef TRANSPORTHANDLE_H
#define TRANSPORTHANDLE_H

#include <QString>
#include <QtGui/QImage>

namespace ds {
namespace core {

struct TransportHandle {

    QString identityName;

    // Handle in clear text
    QByteArray handle;
};

struct TransportHandleReq {
    QString identityName;
};

struct TransportHandleError {
    QString identityName;
    QString explanation;
};

}} // namespaces

#endif // TRANSPORTHANDLE_H
