#ifndef TORSOCKETLISTENER_H
#define TORSOCKETLISTENER_H

#include <memory>

#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include "ds/connectionsocket.h"

namespace ds {
namespace prot {

class TorSocketListener : public QTcpServer
{
    Q_OBJECT
public:
    using on_new_connection_fn_t = std::function<void (ConnectionSocket::ptr_t)>;

    TorSocketListener(on_new_connection_fn_t fn);

protected:
    void incomingConnection(qintptr handle) override;
    on_new_connection_fn_t on_new_connection_fn_;
};

}} // namespaces


#endif // TORSOCKETLISTENER_H
