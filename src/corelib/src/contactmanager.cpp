
#include <memory>
#include <algorithm>

#include "ds/contactmanager.h"
#include "ds/dsengine.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;

ContactManager::ContactManager(QObject &parent)
    : QObject (&parent)
{
    connect(this, &ContactManager::contactAdded,
            this, &ContactManager::onContactAddedLater);
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

        // Remove from model(s)
        emit contactDeleted(uuid);

        lru_cache_.remove(contact);
        registry_.remove(uuid);
    }
}

Contact *ContactManager::addContact(Contact::data_t data)
{
    auto ptr = make_shared<Contact>(*this, -1, false, move(data));
    ptr->addToDb();

    // Add it to the registry and cache
    registry_.add(ptr->getUuid(), ptr);
    touch(ptr);

    LFLOG_NOTICE << "Added Contact " << ptr->getName()
                 << " with handle " << ptr->getHandle()
                 << " as " << ptr->getUuid().toString();

    emit contactAdded(ptr);

    return ptr.get();
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

void ContactManager::onContactAddedLater(const Contact::ptr_t& contact)
{
    if (contact->isAutoConnect() && contact->getIdentity()->isOnline()) {
        contact->connectToContact();
    }
}

}}
