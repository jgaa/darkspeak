
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

Contact::ptr_t ContactManager::getContact(const int dbId)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE id=:id");
    query.bindValue(":id", dbId);
    query.exec();
    if (query.next()) {
        return getContact(query.value(0).toUuid());
    }

    return {};
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
    lru_cache_.touch(contact);
    emit contactTouched(contact);
}

void ContactManager::onContactAddedLater(const Contact::ptr_t& contact)
{
    if (contact->isAutoConnect() && contact->getIdentity()->isOnline()) {
        contact->connectToContact();
    }
}

}}
