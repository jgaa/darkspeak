#ifndef FILESMODEL_H
#define FILESMODEL_H

#include <deque>

#include <QAbstractListModel>

#include "ds/file.h"
#include "ds/conversation.h"
#include "ds/contact.h"

namespace ds {
namespace models {

class FilesModel : public QAbstractListModel
{
    Q_OBJECT

    struct Row {
        Row(const int dbId) : id{dbId} {}
        Row(Row&&) = default;
        Row(const Row&) = default;
        Row& operator = (const Row&) = default;

        mutable core::File::ptr_t file;
        int id = 0;
    };

    using rows_t = std::deque<Row>;
public:
    FilesModel(QObject& parent);

    // Reset the model to track files for this conversation
    Q_INVOKABLE void setConversation(core::Conversation *conversation);

    // Reset the model to track files for this contact
    Q_INVOKABLE void setContact(core::Contact *contact);

    // Reset the model to track files for this identity
    Q_INVOKABLE void setIdentity(core::Identity *identity);

private slots:
    void onFileAdded(const core::File::ptr_t& file);
    void onFileDeleted(const int dbId);

private:
    void queryRows(rows_t& rows);

    rows_t rows_;
    core::Conversation::ptr_t currentConversation_;
    core::Contact::ptr_t currentContact_;
    core::Identity *currentIdentity_ = {};
};

}} // namespaces

#endif // FILESMODEL_H
