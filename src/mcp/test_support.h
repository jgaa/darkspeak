#pragma once
#ifndef DARKSPEAK_WITH_TEST_MCP
#error Test support must never be included in a production build
#endif
#include "service.h"
#include <dsfilemanager/filemanager.h>
namespace ds::core {
class Identity;
}
namespace ds::mcp {
void installTestDiagnostics();
QJsonArray testDiagnostics();
// Only the isolated bootstrap constructs this object. It owns the independent
// controller credential and never puts it in the ordinary service key store.
class TestSupport final : public QObject {
  public:
    TestSupport(core::DsEngine& engine, Service& service, QString input_root, QByteArray credential);
    ~TestSupport();
    static bool isOperation(const QString& name);
    static QList<Tool> controllerTools();
    QString authenticate(const std::string& bearer) const;
    void validate(const QString& name, const QJsonObject& args) const;
    QJsonObject execute(const QString& name, core::Identity* owner, const QJsonObject& args);
    void submit(const QString& name, const QJsonObject& args, std::shared_ptr<Reply> reply);

  private:
    QCoro::Task<void> hashFile(QString transfer, std::shared_ptr<Reply> reply);
    core::DsEngine& engine_;
    Service& service_;
    QString input_root_;
    QByteArray verifier_;
    bool hashing_ = false;
    QHash<QString, QString> probes_;
};
} // namespace ds::mcp
