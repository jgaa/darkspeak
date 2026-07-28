#ifndef FILEDIALOGMODEL_H
#define FILEDIALOGMODEL_H

#include <QObject>
#include <QStringList>

class FileDialogModel final : public QObject
{
    Q_OBJECT

public:
    explicit FileDialogModel(QObject *parent = nullptr);

    Q_INVOKABLE QString openFile(const QString& title = {},
                                 const QString& initialPath = {},
                                 const QStringList& nameFilters = {});
    Q_INVOKABLE QString openFolder(const QString& title = {},
                                   const QString& initialPath = {});
};

#endif // FILEDIALOGMODEL_H
