#pragma once
#include <dsfilemanager/filemanager.h>
#include <QHash>
#include <QSet>
namespace dsvfs {
enum class Tier { Identity, Group, Contact };
enum class Decision { Unspecified, Allow, Deny };
// Navigation is distinct from exposing implicit source directories.
enum class Operation {
    Traverse,
    Inspect,
    List,
    Read,
    Create,
    Write,
    Rename,
    Move,
    Remove,
    Export,
    ConfigureMounts,
    Recurse
};
struct PermissionRule {
    QString path = "/";
    Tier tier = Tier::Identity;
    QUuid principal;
    QHash<Operation, Decision> permissions;
    Decision denied = Decision::Unspecified;
};
struct PermissionPolicy {
    QVector<PermissionRule> rules;
    // All operation defaults must be definitive. ConfigureMounts is always a
    // separate host administrative decision, never a contact permission.
    QHash<Operation, Decision> defaults;
    dsfm::Result<void> validate() const;
    Decision resolve(QUuid contact, const QSet<QUuid> &groups, QStringView path, Operation operation) const;
    bool navigable(QUuid contact, const QSet<QUuid> &groups, QStringView path) const;
};
struct StorageResource {
    QUuid id;
    dsfm::NamespaceId namespace_id;
    QUuid owner_identity; // null: application-wide read-only resource
};
struct StorageAssignment {
    QUuid id;
    QUuid identity;
    QUuid resource;
    dsfm::DirectoryId directory;
    bool allow_external_files = false;
};
struct MountDefinition {
    QUuid id;
    QUuid identity;
    QUuid assignment;
    dsfm::DirectoryId directory;
    QString path;
    Tier tier = Tier::Identity;
    QUuid principal;
};
struct OverlayResult {
    QVector<MountDefinition> mounts;
    QSet<QString> conflicts;
};
bool validVirtualPath(QStringView path);
bool pathContains(QStringView ancestor, QStringView path);
// Pure bounded resolver; inputs are locally configured and validated first.
dsfm::Result<OverlayResult> resolveOverlay(const QVector<MountDefinition> &definitions, QUuid identity, QUuid contact,
                                           const QSet<QUuid> &groups);
} // namespace dsvfs
