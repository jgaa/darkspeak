#include <dsfilemanager/filemanager.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QCoroTask>
#include <iostream>

QCoro::Task<void> example(QCoreApplication* app) {
    QTemporaryDir directory;
    dsfm::NativeFileManager manager;
    dsfm::NamespaceConfig config{QUuid::createUuid(), directory.path() + "/files"};
    auto ns = co_await manager.createNamespace(config);
    if (!ns) { app->exit(1); co_return; }
    QByteArray bytes("FileManager example\n");
    dsfm::CreateOptions options{ns.value().root, "example.txt"};
    options.content = {quint64(bytes.size()), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    auto created = co_await manager.createFile(config.id, options);
    if (!created) { app->exit(2); co_return; }
    auto pending = std::move(created).value();
    const auto pending_id = pending.info.id;
    const auto file_id = pending.info.file_id;
    auto written = co_await pending.writer->writeAt(0, bytes);
    auto suspended = co_await pending.writer->suspend();
    auto reopened = co_await manager.openPending(config.id, pending_id);
    if (!written || !suspended || !reopened) { app->exit(3); co_return; }
    auto committed = co_await reopened.value().writer->commit();
    auto listed = co_await manager.list(config.id, ns.value().root);
    auto reader = co_await manager.openRead(config.id, file_id);
    if (!committed || !listed || !reader) { app->exit(4); co_return; }
    auto read = co_await reader.value()->readAt(0, 1024);
    if (read) std::cout << read.value().toStdString();
    co_await reader.value()->close();
    auto stopped = co_await manager.shutdown();
    app->exit(read && stopped ? 0 : 5);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QMetaObject::invokeMethod(&app, [&app] { example(&app); }, Qt::QueuedConnection);
    return app.exec();
}
