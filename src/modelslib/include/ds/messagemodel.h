#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <deque>

#include <QSettings>
#include <QAbstractListModel>

#include "ds/identity.h"
#include "ds/message.h"
#include "ds/conversation.h"

namespace ds {
namespace models {

class MessageModel : public QAbstractListModel
{
    Q_OBJECT

    struct Row {
        Row(int idVal) : id{idVal} {}
        Row(int idVal, std::shared_ptr<core::MessageContent> data) : id{idVal}, data_{std::move(data)} {}
        Row(Row&&) = default;
        Row(const Row&) = default;
        Row& operator = (const Row&) = default;

        int id;
        mutable std::shared_ptr<core::MessageContent> data_;
    };

    enum Cols {
        H_ID = Qt::UserRole, H_CONTENT, H_COMPOSED, H_DIRECTION, H_RECEIVED
    };
public:

    using rows_t = std::deque<Row>;

    MessageModel(QObject& parent);

    Q_INVOKABLE void setConversation(core::Conversation *conversation);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent) const override;
    QHash<int, QByteArray> roleNames() const override;


private slots:
    void onMessageAdded(const core::Message::ptr_t& message);
    void onMessageDeleted(const core::Message::ptr_t& message);
    void onMessageReceivedDateChanged(const core::Message::ptr_t& message);

private:
    void queryRows(rows_t& rows);
    std::shared_ptr<core::MessageContent> loadData(const int id) const;
    std::shared_ptr<core::MessageContent> loadData(const core::Message& message) const;

    rows_t rows_;
    core::Conversation::ptr_t conversation_;
};

}} // namespaces

#endif // MESSAGEMODEL_H
