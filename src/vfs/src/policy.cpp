#include <dsvfs/policy.h>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
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
    Decision operation = Decision::Unspecified;
    bool barrier = false;
    qsizetype depth = -1;
};
QByteArray principalKey(Tier tier, QUuid principal) { return QByteArray(1, char(tier)) + principal.toRfc4122(); }
void contribute(Resolved &result, const PermissionRule &rule, Operation operation) {
    const auto decision = rule.permissions.value(operation, Decision::Unspecified);
    if (decision != Decision::Unspecified && rule.path.size() >= result.depth) {
        result.operation = rule.path.size() == result.depth ? merge(result.operation, decision) : decision;
        result.depth = rule.path.size();
    }
    result.barrier |= rule.denied == Decision::Deny;
}
} // namespace
dsfm::Result<void> PermissionPolicy::validate() const {
    if (rules.size() > 4096 || !generation)
        return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
    if (defaults.size() != int(Operation::Recurse) + 1)
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    for (int op = 0; op <= int(Operation::Recurse); ++op) {
        auto value = defaults.value(Operation(op), Decision::Unspecified);
        if (value != Decision::Allow && value != Decision::Deny)
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    }
    QSet<QUuid> identifiers;
    for (const auto &rule : rules) {
        if (rule.id.isNull() || identifiers.contains(rule.id))
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        identifiers.insert(rule.id);
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
dsfm::Result<QByteArray> PermissionPolicy::serialize() const {
    auto valid = validate();
    if (!valid)
        return valid.error();
    QJsonArray default_values, records;
    for (int op = 0; op <= int(Operation::Recurse); ++op)
        default_values.append(int(defaults.value(Operation(op))));
    for (const auto &rule : rules) {
        QJsonArray permissions;
        for (int op = 0; op <= int(Operation::Recurse); ++op)
            permissions.append(int(rule.permissions.value(Operation(op), Decision::Unspecified)));
        records.append(QJsonObject{{"id", rule.id.toString(QUuid::WithoutBraces)},
                                   {"path", rule.path},
                                   {"tier", int(rule.tier)},
                                   {"principal", rule.principal.toString(QUuid::WithoutBraces)},
                                   {"permissions", permissions},
                                   {"denied", int(rule.denied)}});
    }
    auto bytes = QJsonDocument(QJsonObject{{"version", int(authorization_format_version)},
                                           {"generation", QString::number(generation)},
                                           {"defaults", default_values},
                                           {"rules", records}})
                     .toJson(QJsonDocument::Compact);
    if (bytes.size() > (4 << 20))
        return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
    return bytes;
}
dsfm::Result<PermissionPolicy> PermissionPolicy::deserialize(QByteArray bytes) {
    if (bytes.size() > (4 << 20))
        return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
    auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto object = document.object();
    if (object.size() != 4 || object["version"] != int(authorization_format_version) || !object["defaults"].isArray() ||
        !object["rules"].isArray())
        return dsfm::Error{dsfm::ErrorCode::UnsupportedCapability};
    PermissionPolicy policy;
    bool ok = false;
    policy.generation = object["generation"].toString().toULongLong(&ok);
    const auto defaults = object["defaults"].toArray(), rules = object["rules"].toArray();
    if (!ok || !policy.generation || defaults.size() != int(Operation::Recurse) + 1 || rules.size() > 4096)
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto decision = [](QJsonValue value) {
        return value.isDouble() && value.toDouble() == value.toInt(-1) && value.toInt(-1) >= 0 && value.toInt(-1) <= 2;
    };
    for (int op = 0; op < defaults.size(); ++op) {
        if (!decision(defaults[op]))
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        policy.defaults[Operation(op)] = Decision(defaults[op].toInt());
    }
    for (const auto &record : rules) {
        if (!record.isObject())
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        auto value = record.toObject();
        PermissionRule rule;
        if (value.size() != 6 || !value["path"].isString() || !value["permissions"].isArray() ||
            !decision(value["denied"]) || !value["tier"].isDouble() || value["tier"].toInt(-1) < 0 ||
            value["tier"].toInt(-1) > 2 || value["tier"].toDouble() != value["tier"].toInt(-1))
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        rule.id = QUuid(value["id"].toString());
        rule.principal = QUuid(value["principal"].toString());
        if (rule.id.toString(QUuid::WithoutBraces) != value["id"].toString() ||
            rule.principal.toString(QUuid::WithoutBraces) != value["principal"].toString())
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        rule.path = value["path"].toString();
        rule.tier = Tier(value["tier"].toInt());
        rule.denied = Decision(value["denied"].toInt());
        const auto permissions = value["permissions"].toArray();
        if (permissions.size() != int(Operation::Recurse) + 1)
            return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        for (int op = 0; op < permissions.size(); ++op) {
            if (!decision(permissions[op]))
                return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
            if (permissions[op].toInt() != int(Decision::Unspecified))
                rule.permissions[Operation(op)] = Decision(permissions[op].toInt());
        }
        policy.rules.append(std::move(rule));
    }
    auto valid = policy.validate();
    if (!valid)
        return valid.error();
    return policy;
}
AuthorizationDecision PermissionPolicy::explain(QUuid contact, const QSet<QUuid> &groups, QStringView path,
                                                Operation op) const {
    AuthorizationDecision answer;
    answer.policy_generation = generation;
    if (!validate())
        return answer;
    if (!validVirtualPath(path) || rules.size() > 4096 || groups.size() > 128 || !generation || int(op) < 0 ||
        int(op) > int(Operation::Recurse) || op == Operation::ConfigureMounts)
        return answer;
    QHash<QByteArray, Resolved> resolved_principals;
    for (const auto &rule : rules)
        if (applicable(rule, contact, groups) && pathContains(rule.path, path))
            contribute(resolved_principals[principalKey(rule.tier, rule.principal)], rule, op);
    for (const auto &rule : rules) {
        if (!applicable(rule, contact, groups) || !pathContains(rule.path, path))
            continue;
        const auto value = rule.permissions.value(op, Decision::Unspecified);
        if (value == Decision::Unspecified && rule.denied == Decision::Unspecified)
            continue;
        auto resolved = resolved_principals.value(principalKey(rule.tier, rule.principal));
        answer.contributions.append({rule.id, rule.tier, rule.principal, rule.path, value, rule.denied,
                                     value != Decision::Unspecified && rule.path.size() == resolved.depth, false,
                                     false});
    }
    const auto fallback = defaults.value(op, Decision::Deny);
    answer.contributions.append({{}, Tier::Identity, {}, "/", fallback, Decision::Unspecified, false, false, true});
    for (auto tier : {Tier::Contact, Tier::Group, Tier::Identity}) {
        Resolved result;
        if (tier == Tier::Group) {
            for (auto group : groups) {
                auto r = resolved_principals.value(principalKey(tier, group));
                result.operation = merge(result.operation, r.operation);
                result.barrier |= r.barrier;
            }
        } else
            result = resolved_principals.value(principalKey(tier, tier == Tier::Contact ? contact : QUuid{}));
        if (!result.barrier && result.operation == Decision::Unspecified)
            continue;
        answer.outcome = result.barrier                        ? AuthorizationOutcome::Hidden
                         : result.operation == Decision::Allow ? AuthorizationOutcome::Allowed
                                                               : AuthorizationOutcome::Denied;
        answer.reason = result.barrier                        ? AuthorizationReason::PathDenied
                        : result.operation == Decision::Allow ? AuthorizationReason::Granted
                                                              : AuthorizationReason::PermissionDenied;
        for (auto &contribution : answer.contributions) {
            if (contribution.tier != tier || contribution.identity_default)
                continue;
            contribution.decisive = result.barrier
                                        ? contribution.barrier == Decision::Deny
                                        : contribution.most_specific && contribution.permission == result.operation;
        }
        return answer;
    }
    answer.outcome = fallback == Decision::Allow ? AuthorizationOutcome::Allowed : AuthorizationOutcome::Denied;
    answer.reason = fallback == Decision::Allow ? AuthorizationReason::Granted : AuthorizationReason::DefaultDenied;
    answer.contributions.last().decisive = true;
    return answer;
}
Decision PermissionPolicy::resolve(QUuid contact, const QSet<QUuid> &groups, QStringView path, Operation op) const {
    return explain(contact, groups, path, op).allowed() ? Decision::Allow : Decision::Deny;
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
