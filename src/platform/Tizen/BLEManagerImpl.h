/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *          Provides an implementation of the BLEManager singleton object
 *          for the Tizen platforms.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <bluetooth.h>
#include <glib.h>

#include <ble/Ble.h>
#include <lib/core/CHIPError.h>
#include <lib/support/BitFlags.h>
#include <lib/support/SetupDiscriminator.h>
#include <platform/CHIPDeviceEvent.h>
#include <system/SystemLayer.h>
#include <system/SystemPacketBuffer.h>

#include "ChipDeviceScanner.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

/**
 * enum Class for BLE Scanning state. CHIP supports Scanning by Discriminator or Address
 */
enum class BleScanState : uint8_t
{
    kNotScanning,
    kScanForDiscriminator,
    kScanForAddress,
    kConnecting,
};

/**
 * Structure for BLE Scanning Configuration
 */
struct BLEScanConfig
{
    // If an active scan for connection is being performed
    BleScanState mBleScanState = BleScanState::kNotScanning;

    // If scanning by discriminator, what are we scanning for
    SetupDiscriminator mDiscriminator;

    // If scanning by address, what address are we searching for
    std::string mAddress;

    // Optional argument to be passed to callback functions provided by the BLE scan/connect requestor
    void * mAppState = nullptr;
};

class BLEConnection
{
public:
    explicit BLEConnection(const char * addr) : peerAddr(addr) {}
    ~BLEConnection() = default;

    std::string peerAddr;
    unsigned int mtu = 0;
    // These are handle copies managed by BLEManagerImpl.
    bt_gatt_h gattCharC1Handle = nullptr;
    bt_gatt_h gattCharC2Handle = nullptr;
};

/**
 * Concrete implementation of the BLEManagerImpl singleton object for the Tizen platforms.
 */
class BLEManagerImpl final : public BLEManager,
                             private Ble::BleLayer,
                             private Ble::BlePlatformDelegate,
                             private Ble::BleApplicationDelegate,
                             private Ble::BleConnectionDelegate,
                             private ChipDeviceScannerDelegate
{
    // Allow the BLEManager interface class to delegate method calls to
    // the implementation methods provided by this class.
    friend BLEManager;

public:
    BLEManagerImpl() : mDeviceScanner(this) {}
    ~BLEManagerImpl() = default;

    CHIP_ERROR ConfigureBle(uint32_t aAdapterId, bool aIsCentral);

private:
    // ===== Members that implement the BLEManager internal interface.

    CHIP_ERROR _Init();
    void _Shutdown();
    bool _IsAdvertisingEnabled();
    CHIP_ERROR _SetAdvertisingEnabled(bool val);
    bool _IsAdvertising();
    CHIP_ERROR _SetAdvertisingMode(BLEAdvertisingMode mode);
    CHIP_ERROR _GetDeviceName(char * buf, size_t bufSize);
    CHIP_ERROR _SetDeviceName(const char * deviceName);
    uint16_t _NumConnections();

    void _OnPlatformEvent(const ChipDeviceEvent * event);
    void HandlePlatformSpecificBLEEvent(const ChipDeviceEvent * event);
    BleLayer * _GetBleLayer();

    // ===== Members that implement virtual methods on BlePlatformDelegate.

    CHIP_ERROR SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                       const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                         const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR CloseConnection(BLE_CONNECTION_OBJECT conId) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT conId) const override;
    CHIP_ERROR SendIndication(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                              System::PacketBufferHandle pBuf) override;
    CHIP_ERROR SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                                System::PacketBufferHandle pBuf) override;

    // ===== Members that implement virtual methods on BleApplicationDelegate.

    void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId) override;

    // ===== Members that implement virtual methods on BleConnectionDelegate.

    void NewConnection(BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator) override;
    void NewConnection(BleLayer * bleLayer, void * appState, BLE_CONNECTION_OBJECT connObj) override{};
    CHIP_ERROR CancelConnection() override;

    //  ===== Members that implement virtual methods on ChipDeviceScannerDelegate
    void OnDeviceScanned(const bt_adapter_le_device_scan_result_info_s & scanInfo,
                         const Ble::ChipBLEDeviceIdentificationInfo & info) override;
    void OnScanComplete() override;
    void OnScanError(CHIP_ERROR err) override;

    // ===== Members for internal use by the following friends.

    friend BLEManager & BLEMgr();
    friend BLEManagerImpl & BLEMgrImpl();

    static BLEManagerImpl sInstance;

    // ===== Private members reserved for use by this class only.

    enum class Flags : uint16_t
    {
        kAsyncInitCompleted       = 0x0001, /**< One-time asynchronous initialization actions have been performed. */
        kTizenBLELayerInitialized = 0x0002, /**< The Tizen Platform BLE layer has been initialized. */
        kAppRegistered            = 0x0004, /**< The CHIPoBLE application has been registered with the Bluez layer. */
        kAdvertisingConfigured    = 0x0008, /**< CHIPoBLE advertising has been configured in the Bluez layer. */
        kAdvertising              = 0x0010, /**< The system is currently CHIPoBLE advertising. */
        kControlOpInProgress      = 0x0020, /**< An async control operation has been issued to the ESP BLE layer. */
        kAdvertisingEnabled       = 0x0040, /**< The application has enabled CHIPoBLE advertising. */
        kFastAdvertisingEnabled   = 0x0080, /**< The application has enabled fast advertising. */
        kUseCustomDeviceName      = 0x0100, /**< The application has configured a custom BLE device name. */
        kAdvertisingRefreshNeeded = 0x0200, /**< The advertising configuration/state in BLE layer needs to be updated. */
    };

    // Minimum and maximum advertising intervals in units of 0.625ms.
    using AdvertisingIntervals = std::pair<uint16_t, uint16_t>;

    CHIP_ERROR _InitImpl();

    void DriveBLEState();

    void AdapterStateChangedCb(int result, bt_adapter_state_e adapterState);
    void AdvertisingStateChangedCb(int result, bt_advertiser_h advertiser, bt_adapter_le_advertising_state_e advState);
    void IndicationStateChangedCb(bool notify, bt_gatt_server_h server, bt_gatt_h gattHandle);
    void ReadValueRequestedCb(const char * remoteAddress, int requestId, bt_gatt_server_h server, bt_gatt_h gattHandle, int offset);
    void WriteValueRequestedCb(const char * remoteAddress, int requestId, bt_gatt_server_h server, bt_gatt_h gattHandle,
                               bool responseNeeded, int offset, const char * value, int len);
    void IndicationConfirmationCb(int result, const char * remoteAddress, bt_gatt_server_h server, bt_gatt_h characteristic,
                                  bool completed);
    void GattConnectionStateChangedCb(int result, bool connected, const char * remoteAddress);
    static void WriteCompletedCb(int result, bt_gatt_h gattHandle, void * userData);
    static void CharacteristicIndicationCb(bt_gatt_h characteristic, char * value, int len, void * userData);

    // ==== BLE Scan.
    void InitiateScan(BleScanState scanType);
    static void HandleScanTimeout(chip::System::Layer *, void * appState);

    // ==== Connection.
    BLEConnection * GetConnection(const char * remoteAddr);
    void AddConnection(const char * remoteAddr);
    void RemoveConnection(const char * remoteAddr);

    void HandleC1CharWrite(BLE_CONNECTION_OBJECT conId, const uint8_t * value, size_t len);
    void HandleC2CharChanged(BLE_CONNECTION_OBJECT conId, const uint8_t * value, size_t len);
    void HandleConnectionEvent(bool connected, const char * remoteAddress);
    static void HandleConnectionTimeout(System::Layer * layer, void * appState);
    bool IsDeviceChipPeripheral(BLE_CONNECTION_OBJECT conId);

    // ==== BLE Adv & GATT Server.
    void NotifyBLEPeripheralGATTServerRegisterComplete(CHIP_ERROR error);
    void NotifyBLEPeripheralAdvConfiguredComplete(CHIP_ERROR error);
    void NotifyBLEPeripheralAdvStartComplete(CHIP_ERROR error);
    void NotifyBLEPeripheralAdvStopComplete(CHIP_ERROR error);
    void NotifyBLESubscribed(BLE_CONNECTION_OBJECT conId, bool indicationsEnabled);
    void NotifyBLEIndicationConfirmation(BLE_CONNECTION_OBJECT conId);
    static void HandleAdvertisingTimeout(chip::System::Layer *, void * appState);
    AdvertisingIntervals GetAdvertisingIntervals() const;

    // ==== Connection.
    CHIP_ERROR ConnectChipThing(const char * address);
    void NotifyBLEConnectionEstablished(BLE_CONNECTION_OBJECT conId);
    void NotifyBLEDisconnection(BLE_CONNECTION_OBJECT conId);
    void NotifyHandleNewConnection(BLE_CONNECTION_OBJECT conId);
    void NotifyHandleConnectFailed(CHIP_ERROR error);
    void NotifyHandleWriteComplete(BLE_CONNECTION_OBJECT conId);
    void NotifySubscribeOpComplete(BLE_CONNECTION_OBJECT conId, bool isSubscribed);

    CHIP_ERROR RegisterGATTServer();
    CHIP_ERROR StartBLEAdvertising();
    CHIP_ERROR StopBLEAdvertising();
    void CleanScanConfig();

    CHIPoBLEServiceMode mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Disabled;
    BitFlags<Flags> mFlags;

    uint32_t mAdapterId = 0;
    bool mIsCentral     = false;

    bt_advertiser_h mAdvertiser = nullptr;
    bool mAdvReqInProgress      = false;

    bt_gatt_h mGattCharC1Handle = nullptr;
    bt_gatt_h mGattCharC2Handle = nullptr;
    std::unordered_map<std::string, BLEConnection *> mConnectionMap;

    ChipDeviceScanner mDeviceScanner;
    BLEScanConfig mBLEScanConfig;

    bt_gatt_client_h mGattClient = nullptr;
};

/**
 * Returns a reference to the public interface of the BLEManager singleton object.
 *
 * Internal components should use this to access features of the BLEManager object
 * that are common to all platforms.
 */
inline BLEManager & BLEMgr()
{
    return BLEManagerImpl::sInstance;
}

/**
 * Returns the platform-specific implementation of the BLEManager singleton object.
 *
 * Internal components can use this to gain access to features of the BLEManager
 * that are specific to the Tizen platforms.
 */
inline BLEManagerImpl & BLEMgrImpl()
{
    return BLEManagerImpl::sInstance;
}

inline Ble::BleLayer * BLEManagerImpl::_GetBleLayer()
{
    return this;
}

inline bool BLEManagerImpl::_IsAdvertisingEnabled()
{
    return mFlags.Has(Flags::kAdvertisingEnabled);
}

inline bool BLEManagerImpl::_IsAdvertising()
{
    return mFlags.Has(Flags::kAdvertising);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
