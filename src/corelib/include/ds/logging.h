#ifndef LOGGING_H
#define LOGGING_H

#include <memory>

#include <QFile>
#include <QSettings>

namespace ds {
namespace core {


class Logging : public QObject
{
    Q_OBJECT
public:
    Logging(QSettings& settings);
    ~Logging();

    static void logMessageHandler(QtMsgType type,
                                  const QMessageLogContext &context,
                                  const QString &msg);

    void onLogMessageHandler(QtMsgType type,
                             const QMessageLogContext &context,
                             const QString &msg);
    static Logging *instance() {
        return instance_;
    }

public slots:
    // Re-open the log-file, applying the current settings
    void changed();

private:
    void open();

    std::unique_ptr<QFile> logFile_;
    static Logging *instance_;
    QSettings& settings_;
};

}} // namespaces

#endif // LOGGING_H
