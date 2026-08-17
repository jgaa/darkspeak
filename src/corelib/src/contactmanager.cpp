
#include <memory>
#include <algorithm>

#include "ds/database.h"
#include "ds/contactmanager.h"
#include "ds/dsengine.h"

#include <QQmlEngine>
#include <QSqlQuery>

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

ContactManager::~ContactManager()
{
}

Contact::ptr_t ContactManager::getContact(const QUuid &uuid)
{
    auto contact = registry_.fetch(uuid);

    if (!contact) {
        try {
            contact = Contact::load(uuid);
        } catch (const std::exception& ex) {
            LFLOG_ERROR << "Failed to load contact " << uuid.toString()
                        << ": " << ex.what()
                        << ". Removing the invalid contact from the database.";

            try {
                Contact::deleteFromDb(uuid);
            } catch (const std::exception& cleanup_ex) {
                LFLOG_ERROR << "Failed to remove invalid contact "
                            << uuid.toString() << ": " << cleanup_ex.what();
            }
            return {};
        }
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
    try {
        Contact::deleteFromDb(uuid);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to delete contact " << uuid.toString()
                    << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QObject::tr("Failed to delete contact: %1")
                .arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
        return;
    }

    if (auto contact = registry_.fetch(uuid)) {
        contact->disconnectFromContact(true);
        lru_cache_.remove(contact);
        registry_.remove(uuid);
    }

    // The database deletion also removes conversations, messages, files and
    // notifications.  Notify all models only after the transaction commits.
    emit contactDeleted(uuid);
}

Contact *ContactManager::addContact(Contact::data_t data)
{
    auto ptr = make_shared<Contact>(-1, false, move(data));
    QQmlEngine::setObjectOwnership(ptr.get(), QQmlEngine::CppOwnership);
    ptr->addToDb();

    // Add it to the registry and cache
    registry_.add(ptr->getUuid(), ptr);
    touch(ptr);

    // Every contact has a default one-to-one conversation.  Keeping this
    // conversation creation here makes the conversations model show newly
    // added contacts immediately, without making the UI responsible for
    // materializing conversations.
    DsEngine::instance().getConversationManager()->getConversation(ptr.get());

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
    if (contact->isAutoConnect()
            && !contact->isBlocked()
            && !contact->isOnline()
            && contact->getIdentity()->isOnline()) {
        contact->connectToContact();
    }
}

}}
