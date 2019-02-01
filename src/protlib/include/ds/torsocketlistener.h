#ifndef TORSOCKETLISTENER_H
#define TORSOCKETLISTENER_H

#include <memory>

#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

namespace ds {
namespace prot {

class TorSocketListener : public QTcpServer
{
public:
    TorSocketListener();

protected:
    void incomingConnection(qintptr handle) override;
};

}} // namespaces

#endif // TORSOCKETLISTENER_H
