#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <deque>

#include <QSettings>
#include <QAbstractListModel>

#include "ds/identity.h"
#include "ds/message.h"
#include "ds/conversation.h"
#include "ds/file.h"

namespace ds {
namespace models {

class MessagesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Type {MESSAGE, FILE};
    Q_ENUM(Type)

private:
    struct Row {
        Row(int idVal, const Type type = MESSAGE) : id{idVal}, type_{type} {}
        Row(int idVal, std::shared_ptr<core::MessageContent> data) : id{idVal}, data_{std::move(data)} {}
        Row(int idVal, core::File::ptr_t file) : id{idVal}, type_{FILE}, file_{std::move(file)} {}
        Row(Row&&) = default;
        Row(const Row&) = default;
        Row& operator = (const Row&) = default;

        bool loaded() const noexcept {
            return ((type_ == MESSAGE) && data_)
                    || ((type_ == FILE) && file_);
        }

        int id;
        Type type_ = MESSAGE;
        mutable std::shared_ptr<core::MessageContent> data_;
        mutable core::File::ptr_t file_;
    };

    enum Cols {
        H_ID = Qt::UserRole, H_CONTENT, H_COMPOSED, H_DIRECTION, H_RECEIVED, H_STATE, H_TYPE, H_FILE, H_STATE_NAME
    };
public:

    using rows_t = std::deque<Row>;

    MessagesModel(QObject& parent);

    Q_INVOKABLE void setConversation(core::Conversation *conversation);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void dataChangedLater();


private slots:
    void onMessageAdded(const core::Message::ptr_t& message);
    void onMessageDeleted(const core::Message::ptr_t& message);
    void onMessageReceivedDateChanged(const core::Message::ptr_t& message);
    void onMessageStateChanged(const core::Message::ptr_t& message);
    void onFileAdded(const core::File::ptr_t& file);
    void onFileDeleted(const int dbId);
    void onFileStateChanged(const core::File *file);

private:
    void queryRows(rows_t& rows);
    void load(Row& row) const;
    std::shared_ptr<core::MessageContent> loadData(const int id) const;
    std::shared_ptr<core::MessageContent> loadData(const core::Message& message) const;
    void onMessageChanged(const core::Message::ptr_t& message, const int role);
    void onFileChanged(const core::File *file, const int role);
    QString getStateName(const Row& r) const;

    mutable rows_t rows_;
    core::Conversation::ptr_t conversation_;    
};

}} // namespaces

#endif // MESSAGEMODEL_H
