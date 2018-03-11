#ifndef PROTMANAGER_H
#define PROTMANAGER_H

#include <memory>

namespace ds {
namespace proto {

class ProtManager
{
public:

    ProtManager();

    static ptr_t create();
};

}} // namespaces

#endif // PROTMANAGER_H
