#pragma once

#include <dsfilemanager/filemanager.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <dsvfs/vfs.h>
#include <QHash>
#include <QImage>
#include <QSettings>
#include <QSet>
#include <QPointer>
#include <QTemporaryDir>
#include <utility>
#include <stop_token>

namespace ds::core {

// Application adapter. Physical paths are only accepted at explicit import/export boundaries.
class ManagedStorage final : public QObject {
    Q_OBJECT
public:
    enum Area { Downloads, Private };
    class Operation {
    public:
        Operation(Operation&& other) noexcept : storage_(std::exchange(other.storage_, nullptr)) {}
        Operation(const Operation&) = delete;
        ~Operation();
    private:
        friend class ManagedStorage;
        explicit Operation(ManagedStorage* storage) : storage_(storage) {}
        ManagedStorage* storage_;
    };
    Operation holdOperation();
    struct Imported {
        dsfm::NamespaceId namespace_id;
        dsfm::FileInfo file;
        QString mime_type;
    };
    explicit ManagedStorage(QSettings& settings, QObject* parent = nullptr);
    QCoro::Task<dsfm::Result<void>> ready();
    QCoro::Task<dsfm::Result<dsvfs::Vfs*>> openVfs();
    dsvfs::Vfs* vfs() const { return vfs_.get(); }
    dsfm::IFileManager& manager() { return *manager_; }
    // Application-wide external-source backend. Hosts supply requester contexts;
    // opening a source never grants a contact access to it.
    dsfm::IFileManager* readOnlyManager() { return readonly_manager_.get(); }
    QCoro::Task<dsfm::Result<dsfm::NamespaceInfo>> openReadOnlySource(dsfm::ReadOnlyNamespaceConfig config, bool create);
    QCoro::Task<dsfm::Result<void>> closeReadOnlySource(dsfm::NamespaceId ns);
    QCoro::Task<dsfm::Result<void>> updateReadOnlyExclusions(dsfm::NamespaceId ns, dsfm::ExclusionPolicy policy);
    dsfm::NamespaceId namespaceId(Area area) const { return configs_[area].id; }
    dsfm::DirectoryId root(Area area) const { return roots_[area]; }
    bool isDownloadNamespace(dsfm::NamespaceId id) const { return download_namespaces_.contains(id); }
    QString downloadsRoot() const { return configs_[Downloads].storage_root; }
    QCoro::Task<dsfm::Result<Imported>> importFile(QString path, QString name, dsfm::CancellationToken cancel = {}, std::function<bool(dsfm::PendingInfo)> reserved = {});
    QCoro::Task<dsfm::Result<QString>> exportFile(dsfm::NamespaceId ns, dsfm::FileId id, QString name, dsfm::CancellationToken cancel = {});
    QCoro::Task<dsfm::Result<QByteArray>> readAsset(QString key);
    QCoro::Task<dsfm::Result<void>> writeAsset(QString key, QByteArray bytes);
    // Bounded, on-demand cache of a complete managed image snapshot. The file
    // row is revalidated before publishing; no physical source path is used.
    QCoro::Task<dsfm::Result<QByteArray>> imagePreview(int file_id);
    static QString filePreview(int file_id, QByteArray sha256);
    void saveAvatar(QString key, QImage image);
    void loadAvatar(QString key, QObject* context, std::function<void(QImage)> apply);
    void removeAsset(QString key);
    QCoro::Task<dsfm::Result<void>> shutdown();
    QCoro::Task<dsfm::Result<void>> collectGarbage();
    static QString identityAvatar(QUuid id);
    static QString contactAvatar(QUuid id);
signals:
    void vfsConflictDetected(QUuid identity, QByteArray key, QString virtual_location);
    void vfsConflictResolved(QUuid identity, QByteArray key);
    void initialized();
    void drained();
    void storageError();
private:
    QCoro::Task<dsfm::Result<void>> writeAssetImpl(QString key, QByteArray bytes, bool preview_lane);
    QCoro::Task<void> saveAvatars(QString key);
    QCoro::Task<void> restoreAvatar(QString key, QPointer<QObject> context, std::function<void(QImage)> apply);
    QCoro::Task<void> eraseAsset(QString key);
    QSettings& settings_;
    std::shared_ptr<dsfm::ReadOnlyFileManager> readonly_manager_;
    std::unique_ptr<dsvfs::Vfs> vfs_;
    bool opening_vfs_ = false;
    QStringList host_private_roots_;
    QString export_root_;
    std::shared_ptr<dsfm::NativeFileManager> manager_ = std::make_shared<dsfm::NativeFileManager>();
    dsfm::NamespaceConfig configs_[2];
    dsfm::DirectoryId roots_[2];
    bool initializing_ = false;
    bool ready_ = false;
    bool stopping_ = false;
    bool shutdown_requested_ = false;
    bool closed_ = false;
    std::optional<dsfm::Error> shutdown_error_;
    bool collecting_ = false;
    std::optional<dsfm::Error> initialization_error_;
    QHash<QString, QByteArray> avatar_queue_;
    QSet<QString> asset_writers_;
    quint32 active_ = 0;
    quint32 preview_calls_ = 0;
    bool preview_busy_ = false;
    std::stop_source preview_stop_;
    QSet<dsfm::NamespaceId> download_namespaces_;
    quint64 export_bytes_ = 0;
    quint32 export_slots_ = 0;
    std::vector<std::unique_ptr<QTemporaryDir>> exports_;
};
} // namespace ds::core
