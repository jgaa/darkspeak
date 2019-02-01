
#include "ds/torsocketlistener.h"

namespace ds {
namespace prot {

using namespace std;

TorSocketListener::TorSocketListener(TorSocketListener::on_new_connection_fn_t fn)
    : on_new_connection_fn_{move(fn)}
{

}

void TorSocketListener::incomingConnection(qintptr handle)
{
    auto connection = make_shared<ConnectionSocket>();
    connection->setSocketDescriptor(handle);

    on_new_connection_fn_(move(connection));
}


}} // namespaces
