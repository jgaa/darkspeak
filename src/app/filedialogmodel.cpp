#include "filedialogmodel.h"

#include <QFileDialog>

FileDialogModel::FileDialogModel(QObject *parent)
    : QObject{parent}
{
}

QString FileDialogModel::openFile(const QString& title,
                                  const QString& initialPath,
                                  const QStringList& nameFilters)
{
    return QFileDialog::getOpenFileName(nullptr, title, initialPath,
                                        nameFilters.join(QStringLiteral(";;")));
}

QString FileDialogModel::openFolder(const QString& title,
                                    const QString& initialPath)
{
    return QFileDialog::getExistingDirectory(nullptr, title, initialPath);
}
