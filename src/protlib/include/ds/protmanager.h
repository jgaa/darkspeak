#ifndef PROTMANAGER_H
#define PROTMANAGER_H

#include <memory>

namespace ds {
namespace proto {

class ProtManager
{
public:
    using ptr_t = std::shared_ptr<ProtManager>;

    ProtManager();

    static ptr_t create();
};

}} // namespaces

#endif // PROTMANAGER_H
