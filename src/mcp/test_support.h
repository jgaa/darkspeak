#pragma once
#ifndef DARKSPEAK_WITH_TEST_MCP
#error Test support must never be included in a production build
#endif
#include "service.h"
#include <dsfilemanager/filemanager.h>
namespace ds::core {
class Identity;
class File;
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
    std::shared_ptr<core::File> previewFile(int id);
    QCoro::Task<void> inspectStorage(QString transfer, std::shared_ptr<Reply> reply);
    QCoro::Task<void> hashFile(QString transfer, std::shared_ptr<Reply> reply);
    core::DsEngine& engine_;
    Service& service_;
    QString input_root_;
    QByteArray verifier_;
    bool hashing_ = false;
    bool inspecting_ = false;
    QHash<QString, QString> probes_;
    QHash<QString, qlonglong> resumed_offsets_;
    struct BioWireStats {
        int responses = 0, content_frames = 0, hash_frames = 0, forbidden_frames = 0;
    };
    QHash<QUuid, BioWireStats> bio_wire_;
    // Headless equivalent of a bounded visible conversation: retain file
    // objects long enough to observe their asynchronous preview result.
    QHash<int, std::shared_ptr<core::File>> preview_files_;
};
} // namespace ds::mcp
