#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <deque>
#include <QThread>
#include <QHash>
#include <QSet>

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
    Q_PROPERTY(bool loading READ loading NOTIFY historyChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY historyChanged)
    Q_PROPERTY(bool hasNewer READ hasNewer NOTIFY historyChanged)
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
    Q_PROPERTY(QString historyError READ historyError NOTIFY historyChanged)

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

        QString sort_created;
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
    ~MessagesModel() override;
    bool loading() const { return loading_; }
    bool hasOlder() const { return has_older_; }
    bool hasNewer() const { return has_newer_; }
    QString historyError() const { return history_error_; }
    QString draft() const;
    void setDraft(const QString &text);
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void loadNewer();
    Q_INVOKABLE void loadLatest();

    Q_INVOKABLE void setConversation(core::Conversation *conversation);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void dataChangedLater();
    void historyChanged();
    void draftChanged();
    void pageLoaded(int direction);


private slots:
    void onContactDeleted(const QUuid& uuid);
    void onMessageAdded(const core::Message::ptr_t& message);
    void onMessageDeleted(const core::Message::ptr_t& message);
    void onMessageReceivedDateChanged(const core::Message::ptr_t& message);
    void onMessageStateChanged(const core::Message::ptr_t& message);
    void onFileAdded(const core::File::ptr_t& file);
    void onFileDeleted(const int dbId);
    void onFileStateChanged(const core::File *file);

private:
    void trimLiveWindow();
    void insertLiveRow(Row row);
    void recordPendingMessage(const core::Message::ptr_t &message);
    void recordPendingDeletion(int id, Type type);
    void requestPage(int direction);
    void acceptPage(quint64 generation, int direction, rows_t rows, bool more, const QString &error);
    std::shared_ptr<core::MessageContent> loadData(const core::Message& message) const;
    void onMessageChanged(const core::Message::ptr_t& message, const int role);
    void onFileChanged(const core::File *file, const int role);
    QString getStateName(const Row& r) const;

    mutable rows_t rows_;
    QThread history_thread_;
    QObject *history_worker_ = nullptr;
    quint64 generation_ = 0;
    bool loading_ = false;
    QHash<int, core::Message::ptr_t> pending_messages_;
    QSet<quint64> pending_deletions_;
    bool has_older_ = false;
    bool has_newer_ = false;
    QString history_error_;
    QHash<int, QString> drafts_;
    std::deque<int> draft_order_;
    QMetaObject::Connection policy_connection_;
    core::Conversation::ptr_t conversation_;    
};

}} // namespaces

#endif // MESSAGEMODEL_H
