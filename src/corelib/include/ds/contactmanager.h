#ifndef CONTACTMANAGER_H
#define CONTACTMANAGER_H

#include <deque>
#include <unordered_map>
#include <QUuid>
#include <QObject>

#include "ds/contact.h"
#include "ds/identity.h"
#include "ds/registry.h"
#include "ds/lru_cache.h"


namespace ds {
namespace core {

class ContactManager : public QObject
{
    Q_OBJECT
public:
    ContactManager(QObject& parent);
    ~ContactManager() override;

    Contact::ptr_t getContact(const QUuid& uuid);
    Contact::ptr_t getContact(const int dbId);
    void deleteContact(const QUuid& uuid);
    Contact *addContact(Contact::data_t data);

    // Put the contact at the front of the lru cache
    void touch(const Contact::ptr_t& contact);

signals:
    void contactAdded(const Contact::ptr_t& contact);
    void contactDeleted(const QUuid& contact);
    void contactTouched(const Contact::ptr_t& contact);

private slots:
    void onContactAddedLater(const Contact::ptr_t& contact);

private:
    Registry<QUuid, Contact> registry_;
    LruCache<Contact::ptr_t> lru_cache_{7};
};

}}

#endif // CONTACTMANAGER_H
