#ifndef CONVERSATIONSMODEL_H
#define CONVERSATIONSMODEL_H

#include <QSettings>
#include <QSqlTableModel>
#include <QImage>
#include <QMetaType>

#include "ds/contact.h"

namespace ds {
namespace models {

class ConversationsModel : public QSqlTableModel
{
public:
    enum class Type {
        PRIVATE_P2P,
    };

    ConversationsModel(QSettings& settings);

    bool keyExists(QByteArray key) const;

public slots:
    void onConversationCreated(/*const ds::core::Contact& contact*/);

private:
    int h_id_ = {};
    int h_identity_ = {};
    int h_type_ = {};
    int h_uuid_ = {};
    int h_key_ = {};
    int h_participants_ = {};
    int h_topic_ = {};
    int h_name_= {};

    QSettings& settings_;

};

}} // namespaces

#endif // CONVERSATIONSMODEL_H
