#include "platform/powerdevices.h"
#include <QHash>
#include <QSet>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bluetoothapis.h>
#  include <bthdef.h>
#  include <functiondiscovery.h>
#  include <functiondiscoverycategories.h>
#  include <propkey.h>
#  include <propsys.h>
#endif

namespace PowerDevices {
namespace {
#ifdef Q_OS_WIN
QString normalizedDeviceName(const QString& value)
{
    QString result;
    result.reserve(value.size());
    for (const QChar ch : value.toLower()) {
        if (ch.isLetterOrNumber())
            result.append(ch);
    }
    return result;
}

int batteryValue(const PROPVARIANT& value)
{
    switch (value.vt) {
    case VT_UI1: return value.bVal;
    case VT_I1:  return value.cVal;
    case VT_UI2: return value.uiVal;
    case VT_I2:  return value.iVal;
    case VT_UI4: return static_cast<int>(value.ulVal);
    case VT_I4:  return value.lVal;
    default:     return -1;
    }
}

QString stringValue(const PROPVARIANT& value)
{
    if (value.vt == VT_LPWSTR && value.pwszVal)
        return QString::fromWCharArray(value.pwszVal);
    if (value.vt == VT_BSTR && value.bstrVal)
        return QString::fromWCharArray(value.bstrVal);
    return {};
}

struct WindowsPeripheralProperty {
    QString name;
    QString category;
    int level = -1;
    bool connected = false;
};

QString stringListValue(const PROPVARIANT& value)
{
    if (value.vt == (VT_VECTOR | VT_LPWSTR)) {
        QStringList parts;
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i) {
            if (value.calpwstr.pElems[i])
                parts.append(QString::fromWCharArray(value.calpwstr.pElems[i]));
        }
        return parts.join(QChar(' '));
    }
    return stringValue(value);
}

bool boolValue(const PROPVARIANT& value)
{
    return (value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE)
           || (value.vt == VT_UI1 && value.bVal != 0)
           || (value.vt == VT_UI4 && value.ulVal != 0);
}

QVector<WindowsPeripheralProperty> windowsPeripheralProperties()
{
    QVector<WindowsPeripheralProperty> result;
    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = apartmentResult == S_OK || apartmentResult == S_FALSE;

    IFunctionDiscovery* discovery = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FunctionDiscovery), nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   __uuidof(IFunctionDiscovery),
                                   reinterpret_cast<void**>(&discovery)))) {
        IFunctionInstanceCollection* collection = nullptr;
        // 通过 Windows 设备发现读取驱动提供的 System.Devices.BatteryLife。
        if (SUCCEEDED(discovery->GetInstanceCollection(FCTN_CATEGORY_DEVICES,
                                                       nullptr, TRUE,
                                                       &collection))) {
            DWORD count = 0;
            if (SUCCEEDED(collection->GetCount(&count))) {
                count = qMin<DWORD>(count, 512);
                for (DWORD i = 0; i < count; ++i) {
                    IFunctionInstance* instance = nullptr;
                    if (FAILED(collection->Item(i, &instance)) || !instance)
                        continue;

                    IPropertyStore* properties = nullptr;
                    if (SUCCEEDED(instance->OpenPropertyStore(STGM_READ, &properties))) {
                        PROPVARIANT name{}, category{}, level{}, connected{};
                        PropVariantInit(&name);
                        PropVariantInit(&category);
                        PropVariantInit(&level);
                        PropVariantInit(&connected);
                        if (SUCCEEDED(properties->GetValue(PKEY_Devices_FriendlyName, &name))) {
                            WindowsPeripheralProperty device;
                            device.name = stringValue(name).trimmed();
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_Category, &category)))
                                device.category = stringListValue(category);
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_BatteryLife, &level))) {
                                const int percentage = batteryValue(level);
                                if (percentage >= 0 && percentage <= 100)
                                    device.level = percentage;
                            }
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_Connected, &connected)))
                                device.connected = boolValue(connected);
                            // 旧驱动未提供连接标志时仍保留电量记录，后续可与蓝牙枚举结果关联。
                            if (!device.name.isEmpty() && (device.connected || device.level >= 0))
                                result.append(device);
                        }
                        PropVariantClear(&connected);
                        PropVariantClear(&level);
                        PropVariantClear(&category);
                        PropVariantClear(&name);
                        properties->Release();
                    }
                    instance->Release();
                }
            }
            collection->Release();
        }
        discovery->Release();
    }

    if (uninitialize)
        CoUninitialize();
    return result;
}

int iconKindForBluetoothDevice(const QString& name, ULONG classOfDevice)
{
    const QString lowered = name.toLower();
    if (lowered.contains(QStringLiteral("mouse")) || lowered.contains(QStringLiteral("鼠标")))
        return 1;
    if (lowered.contains(QStringLiteral("keyboard")) || lowered.contains(QStringLiteral("键盘")))
        return 2;
    if (lowered.contains(QStringLiteral("head")) || lowered.contains(QStringLiteral("airpod"))
        || lowered.contains(QStringLiteral("buds")) || lowered.contains(QStringLiteral("耳机")))
        return 3;

    const ULONG major = GET_COD_MAJOR(classOfDevice);
    const ULONG minor = GET_COD_MINOR(classOfDevice);
    if (major == COD_MAJOR_PERIPHERAL) {
        if ((minor & COD_PERIPHERAL_MINOR_POINTER_MASK) != 0)
            return 1;
        if ((minor & COD_PERIPHERAL_MINOR_KEYBOARD_MASK) != 0)
            return 2;
    }
    if (major == COD_MAJOR_AUDIO)
        return 3;
    return 4;
}
#endif
}

Status powerStatus()
{
    Status result;
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS native{};
    if (GetSystemPowerStatus(&native)) {
        result.hasBattery = native.BatteryFlag != 128 && native.BatteryLifePercent != 255;
        if (result.hasBattery)
            result.level = qBound(0, int(native.BatteryLifePercent), 100);
        result.charging = result.hasBattery && native.ACLineStatus == 1;
    }
#endif
    return result;
}

QVector<Device> connectedDevices(int limit)
{
    QVector<Device> devices;
    if (limit <= 0)
        return devices;
#ifdef Q_OS_WIN
    const QVector<WindowsPeripheralProperty> properties = windowsPeripheralProperties();
    QHash<QString, int> batteryLevels;
    QSet<QString> seen;
    const auto alreadySeen = [&seen](const QString& key) {
        if (seen.contains(key))
            return true;
        if (key.size() < 5)
            return false;
        for (const QString& existing : seen) {
            if (existing.contains(key) || key.contains(existing))
                return true;
        }
        return false;
    };

    // 设备发现补充旧蓝牙接口未返回的低功耗蓝牙和 HID 设备。
    for (const WindowsPeripheralProperty& property : properties) {
        const QString key = normalizedDeviceName(property.name);
        if (key.isEmpty())
            continue;
        if (property.level >= 0)
            batteryLevels.insert(key, property.level);
        if (!property.connected || devices.size() >= limit || alreadySeen(key))
            continue;

        const QString description = property.name + QChar(' ') + property.category;
        const int iconKind = iconKindForBluetoothDevice(description, 0);
        const QString lowered = description.toLower();
        const bool internalBattery = lowered.contains(QStringLiteral("acpi"))
                                     || lowered.contains(QStringLiteral("control method battery"))
                                     || lowered.contains(QStringLiteral("电池"));
        // 只展示可识别的输入、音频配件或实际报告电量的设备，排除普通显示器和打印机。
        if (internalBattery || (iconKind == 4 && property.level < 0))
            continue;

        Device device;
        device.name = property.name;
        device.iconKind = iconKind;
        device.level = property.level;
        devices.append(device);
        seen.insert(key);
    }

    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof(search);
    search.fReturnConnected = TRUE;
    search.fIssueInquiry = FALSE;
    search.cTimeoutMultiplier = 1;

    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    HBLUETOOTH_DEVICE_FIND finder = BluetoothFindFirstDevice(&search, &info);
    if (finder && devices.size() < limit) {
        do {
            if (!info.fConnected)
                continue;
            const QString name = QString::fromWCharArray(info.szName).trimmed();
            const QString key = normalizedDeviceName(name);
            if (key.isEmpty() || alreadySeen(key))
                continue;

            Device device;
            device.name = name;
            device.iconKind = iconKindForBluetoothDevice(name, info.ulClassofDevice);
            device.level = batteryLevels.value(key, -1);
            if (device.level < 0) {
                // 设备名可能带不同后缀，规范化后使用包含匹配，同时避免关联无关短名称。
                for (auto it = batteryLevels.cbegin(); it != batteryLevels.cend(); ++it) {
                    if (key.size() >= 5 && (key.contains(it.key()) || it.key().contains(key))) {
                        device.level = it.value();
                        break;
                    }
                }
            }
            devices.append(device);
            seen.insert(key);
        } while (devices.size() < limit
                 && BluetoothFindNextDevice(finder, &info));
    }
    if (finder)
        BluetoothFindDeviceClose(finder);

#endif
    return devices;
}

}
