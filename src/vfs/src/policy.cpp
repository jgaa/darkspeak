#include <dsvfs/policy.h>
#include <algorithm>
namespace dsvfs {
bool pathContains(QStringView ancestor, QStringView path) {
    return ancestor == u"/" || path == ancestor ||
           (path.startsWith(ancestor) && path.size() > ancestor.size() && path[ancestor.size()] == u'/');
}
bool validVirtualPath(QStringView path) {
    if (path.isEmpty() || path.size() > 4096 || !path.startsWith(u'/') || !path.toString().isValidUtf16())
        return false;
    if (path == u"/")
        return true;
    for (auto component : path.sliced(1).split(u'/')) {
        if (component.isEmpty() || component == u"." || component == u".." ||
            component.toUtf8().size() > dsfm::max_encoded_linux_name_bytes)
            return false;
        for (auto c : component)
            if (c.isNull() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format)
                return false;
    }
    return true;
}
namespace {
bool applicable(const PermissionRule &rule, QUuid contact, const QSet<QUuid> &groups) {
    return rule.tier == Tier::Identity ||
           (rule.tier == Tier::Contact ? rule.principal == contact : groups.contains(rule.principal));
}
Decision merge(Decision a, Decision b) {
    if (a == Decision::Deny || b == Decision::Deny)
        return Decision::Deny;
    return a == Decision::Allow || b == Decision::Allow ? Decision::Allow : Decision::Unspecified;
}
struct Resolved {
    Decision operation = Decision::Unspecified, denied = Decision::Unspecified;
};
Resolved atTier(const PermissionPolicy &policy, Tier tier, QUuid principal, QStringView path, Operation operation) {
    Resolved result;
    qsizetype op_depth = -1, deny_depth = -1;
    for (const auto &rule : policy.rules) {
        if (rule.tier != tier || rule.principal != principal || !pathContains(rule.path, path))
            continue;
        auto decision = rule.permissions.value(operation, Decision::Unspecified);
        if (decision != Decision::Unspecified && rule.path.size() >= op_depth) {
            result.operation = rule.path.size() == op_depth ? merge(result.operation, decision) : decision;
            op_depth = rule.path.size();
        }
        if (rule.denied != Decision::Unspecified && rule.path.size() >= deny_depth) {
            result.denied = rule.path.size() == deny_depth ? merge(result.denied, rule.denied) : rule.denied;
            deny_depth = rule.path.size();
        }
    }
    return result;
}
} // namespace
dsfm::Result<void> PermissionPolicy::validate() const {
    if (rules.size() > 4096)
        return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
    for (int op = 0; op <= int(Operation::Recurse); ++op) {
        auto value = defaults.value(Operation(op), Decision::Unspecified);
        if (value != Decision::Allow && value != Decision::Deny)
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    }
    for (const auto &rule : rules) {
        if (!validVirtualPath(rule.path) || int(rule.tier) < 0 || int(rule.tier) > 2 || int(rule.denied) < 0 ||
            int(rule.denied) > 2 || (rule.tier == Tier::Identity ? !rule.principal.isNull() : rule.principal.isNull()))
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        for (auto it = rule.permissions.cbegin(); it != rule.permissions.cend(); ++it)
            if (int(it.key()) < 0 || int(it.key()) > int(Operation::Recurse) || int(it.value()) < 0 ||
                int(it.value()) > 2 || it.key() == Operation::ConfigureMounts)
                return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    }
    return {};
}
Decision PermissionPolicy::resolve(QUuid contact, const QSet<QUuid> &groups, QStringView path, Operation op) const {
    if (op == Operation::Export)
        op = Operation::List;
    if (!validVirtualPath(path) || rules.size() > 4096 || groups.size() > 128 || op == Operation::ConfigureMounts)
        return Decision::Deny;
    for (auto tier : {Tier::Contact, Tier::Group, Tier::Identity}) {
        Resolved result;
        if (tier == Tier::Group) {
            for (auto group : groups) {
                auto r = atTier(*this, tier, group, path, op);
                result.operation = merge(result.operation, r.operation);
                result.denied = merge(result.denied, r.denied);
            }
        } else
            result = atTier(*this, tier, tier == Tier::Contact ? contact : QUuid{}, path, op);
        if (result.denied == Decision::Deny)
            return Decision::Deny;
        if (result.operation != Decision::Unspecified)
            return result.operation;
    }
    return defaults.value(op, Decision::Deny);
}
bool PermissionPolicy::navigable(QUuid contact, const QSet<QUuid> &groups, QStringView path) const {
    if (!validVirtualPath(path))
        return false;
    if (resolve(contact, groups, path, Operation::Traverse) == Decision::Allow)
        return true;
    for (const auto &rule : rules) {
        if (!applicable(rule, contact, groups) || rule.path == path || !pathContains(path, rule.path))
            continue;
        for (auto op : {Operation::Inspect, Operation::List, Operation::Read})
            if (rule.permissions.value(op, Decision::Unspecified) == Decision::Allow &&
                resolve(contact, groups, rule.path, op) == Decision::Allow)
                return true;
    }
    return false;
}
dsfm::Result<OverlayResult> resolveOverlay(const QVector<MountDefinition> &definitions, QUuid identity, QUuid contact,
                                           const QSet<QUuid> &groups) {
    if (definitions.size() > 1024 || groups.size() > 128)
        return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
    QHash<QString, QVector<MountDefinition>> chosen;
    for (auto definition : definitions) {
        if (definition.identity != identity ||
            (definition.tier == Tier::Group && !groups.contains(definition.principal)) ||
            (definition.tier == Tier::Contact && definition.principal != contact))
            continue;
        if (!validVirtualPath(definition.path) || definition.path == u"/" || definition.id.isNull() ||
            definition.assignment.isNull() || definition.directory.isNull() || int(definition.tier) < 0 ||
            int(definition.tier) > 2)
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        auto &bucket = chosen[definition.path];
        if (bucket.isEmpty() || int(bucket.front().tier) < int(definition.tier))
            bucket = {definition};
        else if (bucket.front().tier == definition.tier)
            bucket.append(definition);
    }
    auto paths = chosen.keys();
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) { return a.toUtf8() < b.toUtf8(); });
    OverlayResult result;
    for (auto path : paths) {
        auto bucket = chosen.value(path);
        bool shadowed = false;
        for (auto ancestor : paths) {
            if (ancestor == path || !pathContains(ancestor, path))
                continue;
            if (int(chosen.value(ancestor).front().tier) > int(bucket.front().tier) ||
                (result.conflicts.contains(ancestor) &&
                 int(chosen.value(ancestor).front().tier) >= int(bucket.front().tier))) {
                shadowed = true;
                break;
            }
        }
        if (shadowed)
            continue;
        std::sort(bucket.begin(), bucket.end(), [](const MountDefinition &a, const MountDefinition &b) {
            return a.id.toRfc4122() < b.id.toRfc4122();
        });
        bool conflict = false;
        for (const auto &candidate : bucket)
            if (candidate.assignment != bucket.front().assignment || candidate.directory != bucket.front().directory)
                conflict = true;
        if (conflict)
            result.conflicts.insert(path);
        else
            result.mounts.append(bucket.front());
    }
    return result;
}
} // namespace dsvfs
