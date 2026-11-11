#pragma once

#include <QQuickStyle>

namespace ds::ui {
inline void configureControls()
{
    // Native appearance comes from Appearance's palette. The KDE controls
    // style uses Kirigami colors independently and cannot render our themes.
    // Set this before importing Qt Quick Controls; environment overrides of
    // qtquickcontrols2.conf must not change the controls implementation.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
}
} // namespace ds::ui
