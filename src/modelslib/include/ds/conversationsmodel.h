#ifndef CONVERSATIONSMODEL_H
#define CONVERSATIONSMODEL_H

#include <deque>

#include <QSettings>
#include <QAbstractListModel>
#include <QImage>
#include <QMetaType>

#include "ds/conversationmanager.h"
#include "ds/contactmanager.h"
#include "ds/identitymanager.h"
#include "ds/conversation.h"

namespace ds {
namespace models {

class ConversationsModel : public QAbstractListModel
{
    Q_OBJECT

    struct Row {
        Row(QUuid&& uuidVal) : uuid{std::move(uuidVal)} {}
        Row(Row&&) = default;
        Row(const Row&) = default;
        Row& operator = (const Row&) = default;

        mutable core::Conversation::ptr_t conversation;
        QUuid uuid;
    };

    using rows_t = std::deque<Row>;
public:

    ConversationsModel(QObject& parent);

    Q_PROPERTY(int currentRow READ getCurrentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(ds::core::Conversation * current READ getCurrentConversation WRITE setCurrent NOTIFY currentRowChanged)

    // Set the identity to work with
    Q_INVOKABLE void setIdentity(const QUuid& uuid);
    Q_INVOKABLE void setContact(ds::core::Contact *contact);
    Q_INVOKABLE void setCurrent(ds::core::Conversation *conversation);

    int getCurrentRow() const noexcept;
    void setCurrentRow(int row);
    ds::core::Conversation *getCurrentConversation() const;

signals:
    void currentRowChanged();

public slots:
    void onConversationAdded(const core::Conversation::ptr_t& conversation);
    void onConversationTouched(const core::Conversation::ptr_t& conversation);
    void onConversationDeleted(const QUuid& uuid);

    // QAbstractItemModel interface
public:
    int rowCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;


private:
    void queryRows(rows_t& rows);

    rows_t rows_;
    core::IdentityManager& identityManager_;
    core::ConversationManager& conversationManager_;
    core::ContactManager& contactManager_;
    core::Identity *identity_ = {}; // Active identity
    int currentRow_ = -1;
    core::Conversation::ptr_t current_;
    core::Contact *contact_ = {};
};

}} // namespaces

#endif // CONVERSATIONSMODEL_H
