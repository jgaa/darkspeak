#pragma once

#include <QObject>
#include <QByteArray>
#include <QSettings>

class VideoPreviewSupport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool warningRequired READ warningRequired NOTIFY changed)
    Q_PROPERTY(QString installCommand READ installCommand CONSTANT)
public:
    struct Environment {
        bool native_linux = false;
        bool ffmpeg_available = false;
        QByteArray os_release;
    };
    explicit VideoPreviewSupport(QSettings& settings, QObject* parent = nullptr);
    VideoPreviewSupport(QSettings& settings, Environment environment, QObject* parent = nullptr);
    bool warningRequired() const;
    QString installCommand() const { return install_command_; }
    Q_INVOKABLE void dismiss(bool dont_show_again);
    static QString installationCommand(QByteArray os_release);
signals:
    void changed();
private:
    static Environment detect();
    QSettings& settings_;
    const bool missing_native_;
    const QString install_command_;
    bool dismissed_ = false;
};
