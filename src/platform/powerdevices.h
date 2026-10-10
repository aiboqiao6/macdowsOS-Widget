#pragma once
#include <QString>
#include <QVector>

namespace PowerDevices {
struct Device {
    QString name;
    int level = -1;   // 负一表示系统未提供电量。
    int iconKind = 4; // 图标类型：一为鼠标，二为键盘，三为音频，四为通用设备。
    bool operator==(const Device& other) const {
        return name == other.name && level == other.level && iconKind == other.iconKind;
    }
};
struct Status {
    int level = -1;
    bool charging = false;
    bool hasBattery = false;
};
Status powerStatus();
QVector<Device> connectedDevices(int limit);
}
