#pragma once
#include <QList>
#include <QString>
#include <QStringList>

namespace ds::mcp {
enum class Decision { Ask, Allow, Deny };
struct Rule {
    QString key, identity, tool, contact; // Empty contact is explicit all-contact scope.
    Decision decision = Decision::Ask;
};
struct Evaluation {
    bool denied = false;
    bool approval = false;
};
// Pure resolution shared by production and focused tests. Contact Ask forces a
// fresh decision, and matching persistent denials always take precedence.
inline Evaluation evaluate(const QList<Rule>& rules, const QString& key, const QString& identity,
                           const QString& tool, const QStringList& contacts, const QList<Decision>& policies,
                           bool gated) {
    Evaluation result;
    const auto targets = contacts.isEmpty() ? QStringList{QString{}} : contacts;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        bool allowed = false;
        const auto policy = policies.value(i, Decision::Allow);
        if (policy == Decision::Deny)
            result.denied = true;
        for (const auto& rule : rules) {
            if (rule.key != key || rule.identity != identity || rule.tool != tool ||
                (!rule.contact.isEmpty() && rule.contact != targets[i]))
                continue;
            if (rule.decision == Decision::Deny)
                result.denied = true;
            if (rule.decision == Decision::Allow)
                allowed = true;
        }
        result.approval |= policy == Decision::Ask || (gated && !allowed);
    }
    return result;
}
} // namespace ds::mcp
