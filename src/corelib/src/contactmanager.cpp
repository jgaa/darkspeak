#include <algorithm>

#include "include/ds/contactmanager.h"

namespace ds {
namespace core {

ContactManager::ContactManager(QObject &parent)
    : QObject (&parent)
{

}

Contact::ptr_t ContactManager::getContact(const QUuid &uuid)
{
    auto contact = registry_.fetch(uuid);

    if (!contact) {
        contact = Contact::load(*this, uuid);
        registry_.add(uuid, contact);
    }

    touch(contact);
    return contact;
}

void ContactManager::deleteContact(const QUuid &uuid)
{
    if (auto contact = registry_.fetch(uuid)) {
        lru_cache_.remove(contact);

        // Remove from model(s)
        emit contactDeleted(uuid);

        // TODO: Remove from online lists
    }
}

void ContactManager::touch(const Contact::ptr_t &contact)
{
    auto it = find(lru_cache_.begin(), lru_cache_.end(), contact);
    if (it != lru_cache_.end()) {
        // Just relocate existing pointer to front
        auto ptr = move(*it);
        lru_cache_.erase(it);
        lru_cache_.push_front(move(ptr));
    } else {
        // Add the pointer
        lru_cache_.push_front(contact);
        if (lru_cache_.size() >= lru_size_) {
            lru_cache_.pop_back();
        }
    }
}

}}
