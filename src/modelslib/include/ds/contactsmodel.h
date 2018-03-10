#ifndef CONTACTSMODEL_H
#define CONTACTSMODEL_H

#include <QSettings>
#include <QSqlTableModel>
#include <QImage>
#include <QMetaType>

#include "ds/contact.h"

namespace ds {
namespace models {

class ContactsModel : public QSqlTableModel
{
public:
    ContactsModel(QSettings& settings);

    bool hashExists(QByteArray hash) const;

public slots:
    void onContactCreated(const ds::core::Contact& contact);

private:
    int h_id_ = {};
    int h_identity_ = {};
    int h_uuid_ = {};
    int h_hash_ = {};
    int h_name_ = {};
    int h_nickname_ = {};
    int h_pubkey_ = {};
    int h_address_ = {};
    int h_notes_= {};
    int h_group_ = {};
    int h_avatar_ = {};
    int h_created_ = {};
    int h_initiated_by = {};
    int h_last_seen_ = {};
    int h_online_ = {};

    QSettings& settings_;
};

}} // namespaces

#endif // CONTACTSMODEL_H
