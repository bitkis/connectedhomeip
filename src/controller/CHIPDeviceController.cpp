/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2013-2017 Nest Labs, Inc.
 *    All rights reserved.
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
 *      Implementation of CHIP Device Controller, a common class
 *      that implements discovery, pairing and provisioning of CHIP
 *      devices.
 *
 */

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

// module header, comes first
#include <controller/CHIPDeviceController.h>

#if CONFIG_DEVICE_LAYER
#include <platform/CHIPDeviceLayer.h>
#endif

#include <app/InteractionModelEngine.h>
#include <app/server/DataModelHandler.h>
#include <core/CHIPCore.h>
#include <core/CHIPEncoding.h>
#include <core/CHIPSafeCasts.h>
#include <support/Base64.h>
#include <support/CHIPMem.h>
#include <support/CodeUtils.h>
#include <support/ErrorStr.h>
#include <support/ReturnMacros.h>
#include <support/SafeInt.h>
#include <support/TimeUtils.h>
#include <support/logging/CHIPLogging.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

using namespace chip::Inet;
using namespace chip::System;

namespace chip {
namespace Controller {

using namespace chip::Encoding;

constexpr const char kPairedDeviceListKeyPrefix[] = "ListPairedDevices";
constexpr const char kPairedDeviceKeyPrefix[]     = "PairedDevice";

// This macro generates a key using node ID an key prefix, and performs the given action
// on that key.
#define PERSISTENT_KEY_OP(node, keyPrefix, key, action)                                                                            \
    do                                                                                                                             \
    {                                                                                                                              \
        constexpr size_t len = std::extent<decltype(keyPrefix)>::value;                                                            \
        nlSTATIC_ASSERT_PRINT(len > 0, "keyPrefix length must be known at compile time");                                          \
        /* 2 * sizeof(NodeId) to accomodate 2 character for each byte in Node Id */                                                \
        char key[len + 2 * sizeof(NodeId) + 1];                                                                                    \
        nlSTATIC_ASSERT_PRINT(sizeof(node) <= sizeof(uint64_t), "Node ID size is greater than expected");                          \
        snprintf(key, sizeof(key), "%s%" PRIx64, keyPrefix, node);                                                                 \
        action;                                                                                                                    \
    } while (0)

DeviceController::DeviceController()
{
    mState                    = State::NotInitialized;
    mSessionManager           = nullptr;
    mLocalDeviceId            = 0;
    mStorageDelegate          = nullptr;
    mPairedDevicesInitialized = false;
    mListenPort               = CHIP_PORT;
}

CHIP_ERROR DeviceController::Init(NodeId localDeviceId, PersistentStorageDelegate * storageDelegate, System::Layer * systemLayer,
                                  Inet::InetLayer * inetLayer)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    Transport::AdminPairingInfo * admin = nullptr;

    VerifyOrExit(mState == State::NotInitialized, err = CHIP_ERROR_INCORRECT_STATE);

    if (systemLayer != nullptr && inetLayer != nullptr)
    {
        mSystemLayer = systemLayer;
        mInetLayer   = inetLayer;
    }
    else
    {
#if CONFIG_DEVICE_LAYER
#if CHIP_DEVICE_LAYER_TARGET_LINUX && CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
        // By default, Linux device is configured as a BLE peripheral while the controller needs a BLE central.
        err = DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(/* BLE adapter ID */ 0, /* BLE central */ true);
        SuccessOrExit(err);
#endif // CHIP_DEVICE_LAYER_TARGET_LINUX && CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

        err = DeviceLayer::PlatformMgr().InitChipStack();
        SuccessOrExit(err);

        mSystemLayer = &DeviceLayer::SystemLayer;
        mInetLayer   = &DeviceLayer::InetLayer;
#endif // CONFIG_DEVICE_LAYER
    }

    VerifyOrExit(mSystemLayer != nullptr, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(mInetLayer != nullptr, err = CHIP_ERROR_INVALID_ARGUMENT);

    mStorageDelegate = storageDelegate;

    if (mStorageDelegate != nullptr)
    {
        mStorageDelegate->SetDelegate(this);
    }

    mTransportMgr   = chip::Platform::New<DeviceTransportMgr>();
    mSessionManager = chip::Platform::New<SecureSessionMgr>();

#ifdef CHIP_APP_USE_INTERACTION_MODEL
    mExchangeManager = chip::Platform::New<Messaging::ExchangeManager>();
#endif

    err = mTransportMgr->Init(
        Transport::UdpListenParameters(mInetLayer).SetAddressType(Inet::kIPAddressType_IPv6).SetListenPort(mListenPort)
#if INET_CONFIG_ENABLE_IPV4
            ,
        Transport::UdpListenParameters(mInetLayer).SetAddressType(Inet::kIPAddressType_IPv4).SetListenPort(mListenPort)
#endif
    );
    SuccessOrExit(err);

    admin = mAdmins.AssignAdminId(mAdminId, localDeviceId);
    VerifyOrExit(admin != nullptr, err = CHIP_ERROR_NO_MEMORY);

    err = mSessionManager->Init(localDeviceId, mSystemLayer, mTransportMgr, &mAdmins);
    SuccessOrExit(err);

#ifdef CHIP_APP_USE_INTERACTION_MODEL
    err = mExchangeManager->Init(mSessionManager);
    SuccessOrExit(err);
    err = chip::app::InteractionModelEngine::GetInstance()->Init(mExchangeManager);
    SuccessOrExit(err);
#else
    mSessionManager->SetDelegate(this);
#endif

    InitDataModelHandler();

    mState         = State::Initialized;
    mLocalDeviceId = localDeviceId;

    ReleaseAllDevices();

exit:
    return err;
}

CHIP_ERROR DeviceController::Shutdown()
{
    VerifyOrReturnError(mState == State::Initialized, CHIP_ERROR_INCORRECT_STATE);

    ChipLogDetail(Controller, "Shutting down the controller");

    mState = State::NotInitialized;

#if CONFIG_DEVICE_LAYER
    ReturnErrorOnFailure(DeviceLayer::PlatformMgr().Shutdown());
#else
    mSystemLayer->Shutdown();
    mInetLayer->Shutdown();
    chip::Platform::Delete(mSystemLayer);
    chip::Platform::Delete(mInetLayer);
#endif // CONFIG_DEVICE_LAYER

    mSystemLayer = nullptr;
    mInetLayer   = nullptr;

    if (mStorageDelegate != nullptr)
    {
        mStorageDelegate->SetDelegate(nullptr);
        mStorageDelegate = nullptr;
    }

    if (mSessionManager != nullptr)
    {
        mSessionManager->SetDelegate(nullptr);
        chip::Platform::Delete(mSessionManager);
        mSessionManager = nullptr;
    }

    if (mTransportMgr != nullptr)
    {
        chip::Platform::Delete(mTransportMgr);
        mTransportMgr = nullptr;
    }

    ReleaseAllDevices();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceController::SetUdpListenPort(uint16_t listenPort)
{
    if (mState == State::Initialized)
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    mListenPort = listenPort;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceController::GetDevice(NodeId deviceId, const SerializedDevice & deviceInfo, Device ** out_device)
{
    Device * device = nullptr;

    VerifyOrReturnError(out_device != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    uint16_t index = FindDeviceIndex(deviceId);

    if (index < kNumMaxActiveDevices)
    {
        device = &mActiveDevices[index];
    }
    else
    {
        VerifyOrReturnError(mPairedDevices.Contains(deviceId), CHIP_ERROR_NOT_CONNECTED);

        index = GetInactiveDeviceIndex();
        VerifyOrReturnError(index < kNumMaxActiveDevices, CHIP_ERROR_NO_MEMORY);
        device = &mActiveDevices[index];

        CHIP_ERROR err = device->Deserialize(deviceInfo);
        if (err != CHIP_NO_ERROR)
        {
            ReleaseDevice(device);
            ReturnErrorOnFailure(err);
        }

        device->Init(mTransportMgr, mSessionManager, mInetLayer, mListenPort, mAdminId);
    }

    *out_device = device;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceController::GetDevice(NodeId deviceId, Device ** out_device)
{
    CHIP_ERROR err  = CHIP_NO_ERROR;
    Device * device = nullptr;
    uint16_t index  = 0;
    char * buffer   = nullptr;

    VerifyOrExit(out_device != nullptr, err = CHIP_ERROR_INVALID_ARGUMENT);
    index = FindDeviceIndex(deviceId);

    if (index < kNumMaxActiveDevices)
    {
        device = &mActiveDevices[index];
    }
    else
    {
        VerifyOrExit(mStorageDelegate != nullptr, err = CHIP_ERROR_INCORRECT_STATE);

        if (!mPairedDevicesInitialized)
        {
            constexpr uint16_t max_size = CHIP_MAX_SERIALIZED_SIZE_U64(kNumMaxPairedDevices);
            buffer                      = static_cast<char *>(chip::Platform::MemoryAlloc(max_size));
            uint16_t size               = max_size;

            VerifyOrExit(buffer != nullptr, err = CHIP_ERROR_INVALID_ARGUMENT);

            PERSISTENT_KEY_OP(static_cast<uint64_t>(0), kPairedDeviceListKeyPrefix, key,
                              err = mStorageDelegate->GetKeyValue(key, buffer, size));
            SuccessOrExit(err);
            VerifyOrExit(size <= max_size, err = CHIP_ERROR_INVALID_DEVICE_DESCRIPTOR);

            err = SetPairedDeviceList(buffer);
            SuccessOrExit(err);
        }

        VerifyOrExit(mPairedDevices.Contains(deviceId), err = CHIP_ERROR_NOT_CONNECTED);

        index = GetInactiveDeviceIndex();
        VerifyOrExit(index < kNumMaxActiveDevices, err = CHIP_ERROR_NO_MEMORY);
        device = &mActiveDevices[index];

        {
            SerializedDevice deviceInfo;
            uint16_t size = sizeof(deviceInfo.inner);

            PERSISTENT_KEY_OP(deviceId, kPairedDeviceKeyPrefix, key,
                              err = mStorageDelegate->GetKeyValue(key, Uint8::to_char(deviceInfo.inner), size));
            SuccessOrExit(err);
            VerifyOrExit(size <= sizeof(deviceInfo.inner), err = CHIP_ERROR_INVALID_DEVICE_DESCRIPTOR);

            err = device->Deserialize(deviceInfo);
            VerifyOrExit(err == CHIP_NO_ERROR, ReleaseDevice(device));

            device->Init(mTransportMgr, mSessionManager, mInetLayer, mListenPort, mAdminId);
        }
    }

    *out_device = device;

exit:
    if (buffer != nullptr)
    {
        chip::Platform::MemoryFree(buffer);
    }
    if (err != CHIP_NO_ERROR && device != nullptr)
    {
        ReleaseDevice(device);
    }
    return err;
}

CHIP_ERROR DeviceController::ServiceEvents()
{
    VerifyOrReturnError(mState == State::Initialized, CHIP_ERROR_INCORRECT_STATE);

#if CONFIG_DEVICE_LAYER
    ReturnErrorOnFailure(DeviceLayer::PlatformMgr().StartEventLoopTask());
#endif // CONFIG_DEVICE_LAYER

    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceController::ServiceEventSignal()
{
    VerifyOrReturnError(mState == State::Initialized, CHIP_ERROR_INCORRECT_STATE);

#if CONFIG_DEVICE_LAYER && (CHIP_SYSTEM_CONFIG_USE_SOCKETS || CHIP_SYSTEM_CONFIG_USE_NETWORK_FRAMEWORK)
    DeviceLayer::SystemLayer.WakeSelect();
#else
    ReturnErrorOnFailure(CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
#endif // CONFIG_DEVICE_LAYER && (CHIP_SYSTEM_CONFIG_USE_SOCKETS || CHIP_SYSTEM_CONFIG_USE_NETWORK_FRAMEWORK)

    return CHIP_NO_ERROR;
}

void DeviceController::OnNewConnection(SecureSessionHandle session, SecureSessionMgr * mgr)
{
    VerifyOrReturn(mState == State::Initialized, ChipLogError(Controller, "OnNewConnection was called in incorrect state"));

    uint16_t index = FindDeviceIndex(mgr->GetPeerConnectionState(session)->GetPeerNodeId());
    VerifyOrReturn(index < kNumMaxActiveDevices,
                   ChipLogDetail(Controller, "OnNewConnection was called for unknown device, ignoring it."));

    mActiveDevices[index].OnNewConnection(session, mgr);
}

void DeviceController::OnConnectionExpired(SecureSessionHandle session, SecureSessionMgr * mgr)
{
    VerifyOrReturn(mState == State::Initialized, ChipLogError(Controller, "OnConnectionExpired was called in incorrect state"));

    uint16_t index = FindDeviceIndex(session);
    VerifyOrReturn(index < kNumMaxActiveDevices,
                   ChipLogDetail(Controller, "OnConnectionExpired was called for unknown device, ignoring it."));

    mActiveDevices[index].OnConnectionExpired(session, mgr);
}

void DeviceController::OnMessageReceived(const PacketHeader & header, const PayloadHeader & payloadHeader,
                                         SecureSessionHandle session, System::PacketBufferHandle msgBuf, SecureSessionMgr * mgr)
{
    VerifyOrReturn(mState == State::Initialized, ChipLogError(Controller, "OnMessageReceived was called in incorrect state"));
    VerifyOrReturn(header.GetSourceNodeId().HasValue(),
                   ChipLogError(Controller, "OnMessageReceived was called for unknown source node"));

    uint16_t index = FindDeviceIndex(session);
    VerifyOrReturn(index < kNumMaxActiveDevices,
                   ChipLogError(Controller, "OnMessageReceived was called for unknown device object"));

    mActiveDevices[index].OnMessageReceived(header, payloadHeader, session, std::move(msgBuf), mgr);
}

uint16_t DeviceController::GetInactiveDeviceIndex()
{
    uint16_t i = 0;
    while (i < kNumMaxActiveDevices && mActiveDevices[i].IsActive())
        i++;
    if (i < kNumMaxActiveDevices)
    {
        mActiveDevices[i].SetActive(true);
    }

    return i;
}

void DeviceController::ReleaseDevice(Device * device)
{
    device->Reset();
}

void DeviceController::ReleaseDevice(uint16_t index)
{
    if (index < kNumMaxActiveDevices)
    {
        ReleaseDevice(&mActiveDevices[index]);
    }
}

void DeviceController::ReleaseDeviceById(NodeId remoteDeviceId)
{
    for (uint16_t i = 0; i < kNumMaxActiveDevices; i++)
    {
        if (mActiveDevices[i].GetDeviceId() == remoteDeviceId)
        {
            ReleaseDevice(&mActiveDevices[i]);
        }
    }
}

void DeviceController::ReleaseAllDevices()
{
    for (uint16_t i = 0; i < kNumMaxActiveDevices; i++)
    {
        ReleaseDevice(&mActiveDevices[i]);
    }
}

uint16_t DeviceController::FindDeviceIndex(SecureSessionHandle session)
{
    uint16_t i = 0;
    while (i < kNumMaxActiveDevices)
    {
        if (mActiveDevices[i].IsActive() && mActiveDevices[i].IsSecureConnected() && mActiveDevices[i].MatchesSession(session))
        {
            return i;
        }
        i++;
    }
    return i;
}

uint16_t DeviceController::FindDeviceIndex(NodeId id)
{
    uint16_t i = 0;
    while (i < kNumMaxActiveDevices)
    {
        if (mActiveDevices[i].IsActive() && mActiveDevices[i].GetDeviceId() == id)
        {
            return i;
        }
        i++;
    }
    return i;
}

CHIP_ERROR DeviceController::SetPairedDeviceList(const char * serialized)
{
    CHIP_ERROR err  = CHIP_NO_ERROR;
    size_t len      = strlen(serialized) + 1;
    uint16_t lenU16 = static_cast<uint16_t>(len);
    VerifyOrExit(CanCastTo<uint16_t>(len), err = CHIP_ERROR_INVALID_ARGUMENT);
    err = mPairedDevices.DeserializeBase64(serialized, lenU16);

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Controller, "Failed to recreate the device list\n");
    }
    else
    {
        mPairedDevicesInitialized = true;
    }

    return err;
}

void DeviceController::OnValue(const char * key, const char * value) {}

void DeviceController::OnStatus(const char * key, Operation op, CHIP_ERROR err) {}

DeviceCommissioner::DeviceCommissioner()
{
    mPairingDelegate      = nullptr;
    mRendezvousSession    = nullptr;
    mDeviceBeingPaired    = kNumMaxActiveDevices;
    mPairedDevicesUpdated = false;
}

CHIP_ERROR DeviceCommissioner::Init(NodeId localDeviceId, PersistentStorageDelegate * storageDelegate,
                                    DevicePairingDelegate * pairingDelegate, System::Layer * systemLayer,
                                    Inet::InetLayer * inetLayer)
{
    ReturnErrorOnFailure(DeviceController::Init(localDeviceId, storageDelegate, systemLayer, inetLayer));

    mPairingDelegate = pairingDelegate;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceCommissioner::Shutdown()
{
    VerifyOrReturnError(mState == State::Initialized, CHIP_ERROR_INCORRECT_STATE);

    ChipLogDetail(Controller, "Shutting down the commissioner");

    PersistDeviceList();

    if (mRendezvousSession != nullptr)
    {
        chip::Platform::Delete(mRendezvousSession);
        mRendezvousSession = nullptr;
    }

    DeviceController::Shutdown();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceCommissioner::PairDevice(NodeId remoteDeviceId, RendezvousParameters & params)
{
    CHIP_ERROR err                        = CHIP_NO_ERROR;
    Device * device                       = nullptr;
    Transport::PeerAddress udpPeerAddress = Transport::PeerAddress::UDP(Inet::IPAddress::Any);

    Transport::AdminPairingInfo * admin = mAdmins.FindAdmin(mAdminId);

    VerifyOrExit(remoteDeviceId != kAnyNodeId && remoteDeviceId != kUndefinedNodeId, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(mState == State::Initialized, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mDeviceBeingPaired == kNumMaxActiveDevices, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(admin != nullptr, err = CHIP_ERROR_INCORRECT_STATE);

    params.SetAdvertisementDelegate(&mRendezvousAdvDelegate);

    // TODO: We need to specify the peer address for BLE transport in bindings.
    if (params.GetPeerAddress().GetTransportType() == Transport::Type::kBle ||
        params.GetPeerAddress().GetTransportType() == Transport::Type::kUndefined)
    {
#if CONFIG_DEVICE_LAYER && CONFIG_NETWORK_LAYER_BLE
        if (!params.HasBleLayer())
        {
            params.SetBleLayer(DeviceLayer::ConnectivityMgr().GetBleLayer());
            params.SetPeerAddress(Transport::PeerAddress::BLE());
        }
#endif // CONFIG_DEVICE_LAYER && CONFIG_NETWORK_LAYER_BLE
    }
    else if (params.GetPeerAddress().GetTransportType() == Transport::Type::kTcp ||
             params.GetPeerAddress().GetTransportType() == Transport::Type::kUdp)
    {
        udpPeerAddress = Transport::PeerAddress::UDP(params.GetPeerAddress().GetIPAddress(), params.GetPeerAddress().GetPort(),
                                                     params.GetPeerAddress().GetInterface());
    }

    mDeviceBeingPaired = GetInactiveDeviceIndex();
    VerifyOrExit(mDeviceBeingPaired < kNumMaxActiveDevices, err = CHIP_ERROR_NO_MEMORY);
    device = &mActiveDevices[mDeviceBeingPaired];

    mIsIPRendezvous    = (params.GetPeerAddress().GetTransportType() != Transport::Type::kBle);
    mRendezvousSession = chip::Platform::New<RendezvousSession>(this);
    VerifyOrExit(mRendezvousSession != nullptr, err = CHIP_ERROR_NO_MEMORY);
    err = mRendezvousSession->Init(params.SetLocalNodeId(mLocalDeviceId).SetRemoteNodeId(remoteDeviceId), mTransportMgr,
                                   mSessionManager, admin);
    SuccessOrExit(err);

    device->Init(mTransportMgr, mSessionManager, mInetLayer, mListenPort, remoteDeviceId, udpPeerAddress, admin->GetAdminId());

    // TODO: BLE rendezvous and IP rendezvous should have same logic in the future after BLE becomes a transport and network
    // provisiong cluster is ready.
    if (params.GetPeerAddress().GetTransportType() != Transport::Type::kBle)
    {
        mRendezvousSession->OnRendezvousConnectionOpened();
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        // Delete the current rendezvous session only if a device is not currently being paired.
        if (mDeviceBeingPaired == kNumMaxActiveDevices && mRendezvousSession != nullptr)
        {
            chip::Platform::Delete(mRendezvousSession);
            mRendezvousSession = nullptr;
        }

        if (device != nullptr)
        {
            ReleaseDevice(device);
            mDeviceBeingPaired = kNumMaxActiveDevices;
        }
    }

    return err;
}

CHIP_ERROR DeviceCommissioner::PairTestDeviceWithoutSecurity(NodeId remoteDeviceId, const Transport::PeerAddress & peerAddress,
                                                             SerializedDevice & serialized)
{
    CHIP_ERROR err  = CHIP_NO_ERROR;
    Device * device = nullptr;

    SecurePairingUsingTestSecret * testSecurePairingSecret = nullptr;

    VerifyOrExit(peerAddress.GetTransportType() == Transport::Type::kUdp, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(remoteDeviceId != kUndefinedNodeId && remoteDeviceId != kAnyNodeId, err = CHIP_ERROR_INVALID_ARGUMENT);

    VerifyOrExit(mState == State::Initialized, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mDeviceBeingPaired == kNumMaxActiveDevices, err = CHIP_ERROR_INCORRECT_STATE);

    testSecurePairingSecret = chip::Platform::New<SecurePairingUsingTestSecret>();
    VerifyOrExit(testSecurePairingSecret != nullptr, err = CHIP_ERROR_NO_MEMORY);

    mDeviceBeingPaired = GetInactiveDeviceIndex();
    VerifyOrExit(mDeviceBeingPaired < kNumMaxActiveDevices, err = CHIP_ERROR_NO_MEMORY);
    device = &mActiveDevices[mDeviceBeingPaired];

    testSecurePairingSecret->ToSerializable(device->GetPairing());

    device->Init(mTransportMgr, mSessionManager, mInetLayer, mListenPort, remoteDeviceId, peerAddress, mAdminId);

    device->Serialize(serialized);

    OnRendezvousComplete();

exit:
    if (testSecurePairingSecret != nullptr)
    {
        chip::Platform::Delete(testSecurePairingSecret);
    }

    if (err != CHIP_NO_ERROR)
    {
        if (device != nullptr)
        {
            ReleaseDevice(device);
            mDeviceBeingPaired = kNumMaxActiveDevices;
        }
    }

    return err;
}

CHIP_ERROR DeviceCommissioner::StopPairing(NodeId remoteDeviceId)
{
    VerifyOrReturnError(mState == State::Initialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mDeviceBeingPaired < kNumMaxActiveDevices, CHIP_ERROR_INCORRECT_STATE);

    Device * device = &mActiveDevices[mDeviceBeingPaired];
    VerifyOrReturnError(device->GetDeviceId() == remoteDeviceId, CHIP_ERROR_INVALID_DEVICE_DESCRIPTOR);

    if (mRendezvousSession != nullptr)
    {
        chip::Platform::Delete(mRendezvousSession);
        mRendezvousSession = nullptr;
    }

    ReleaseDevice(device);
    mDeviceBeingPaired = kNumMaxActiveDevices;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DeviceCommissioner::UnpairDevice(NodeId remoteDeviceId)
{
    // TODO: Send unpairing message to the remote device.

    if (mStorageDelegate != nullptr)
    {
        PERSISTENT_KEY_OP(remoteDeviceId, kPairedDeviceKeyPrefix, key, mStorageDelegate->DeleteKeyValue(key));
    }

    mPairedDevices.Remove(remoteDeviceId);
    mPairedDevicesUpdated = true;
    ReleaseDeviceById(remoteDeviceId);

    return CHIP_NO_ERROR;
}

void DeviceCommissioner::RendezvousCleanup(CHIP_ERROR status)
{
    if (mRendezvousSession != nullptr)
    {
        chip::Platform::Delete(mRendezvousSession);
        mRendezvousSession = nullptr;
    }

    if (mPairingDelegate != nullptr)
    {
        mPairingDelegate->OnPairingComplete(status);
    }

    // TODO: make mStorageDelegate mandatory once all controller applications implement the interface.
    if (mDeviceBeingPaired != kNumMaxActiveDevices && mStorageDelegate != nullptr)
    {
        // Let's release the device that's being paired.
        // If pairing was successful, its information is
        // already persisted. The application will use GetDevice()
        // method to get access to the device, which will fetch
        // the device information from the persistent storage.
        DeviceController::ReleaseDevice(mDeviceBeingPaired);
    }

    mDeviceBeingPaired = kNumMaxActiveDevices;
}

void DeviceCommissioner::OnRendezvousError(CHIP_ERROR err)
{
    RendezvousCleanup(err);
}

void DeviceCommissioner::OnRendezvousComplete()
{
    VerifyOrReturn(mDeviceBeingPaired < kNumMaxActiveDevices, OnRendezvousError(CHIP_ERROR_INVALID_DEVICE_DESCRIPTOR));

    Device * device = &mActiveDevices[mDeviceBeingPaired];
    mPairedDevices.Insert(device->GetDeviceId());
    mPairedDevicesUpdated = true;
    // We need to kick the device since we are not a valid secure pairing delegate when using IM.
    device->InitCommandSender();

    // mStorageDelegate would not be null for a typical pairing scenario, as Pair()
    // requires a valid storage delegate. However, test pairing usecase, that's used
    // mainly by test applications, do not require a storage delegate. This is to
    // reduce overheads on these tests.
    // Let's make sure the delegate object is available before calling into it.
    if (mStorageDelegate != nullptr)
    {
        SerializedDevice serialized;
        device->Serialize(serialized);
        PERSISTENT_KEY_OP(device->GetDeviceId(), kPairedDeviceKeyPrefix, key,
                          mStorageDelegate->SetKeyValue(key, Uint8::to_const_char(serialized.inner)));
    }

    RendezvousCleanup(CHIP_NO_ERROR);
}

void DeviceCommissioner::OnRendezvousStatusUpdate(RendezvousSessionDelegate::Status status, CHIP_ERROR err)
{
    Device * device = nullptr;
    if (mDeviceBeingPaired >= kNumMaxActiveDevices)
    {
        ExitNow();
    }

    device = &mActiveDevices[mDeviceBeingPaired];
    switch (status)
    {
    case RendezvousSessionDelegate::SecurePairingSuccess:
        ChipLogDetail(Controller, "Remote device completed SPAKE2+ handshake\n");
        mRendezvousSession->GetPairingSession().ToSerializable(device->GetPairing());

        if (!mIsIPRendezvous && mPairingDelegate != nullptr)
        {
            mPairingDelegate->OnNetworkCredentialsRequested(mRendezvousSession);
        }
        break;

    case RendezvousSessionDelegate::SecurePairingFailed:
        ChipLogDetail(Controller, "Remote device failed in SPAKE2+ handshake\n");
        break;

    case RendezvousSessionDelegate::NetworkProvisioningSuccess:
        ChipLogDetail(Controller, "Remote device was assigned an ip address\n");
        device->SetAddress(mRendezvousSession->GetIPAddress());
        break;

    case RendezvousSessionDelegate::NetworkProvisioningFailed:
        ChipLogDetail(Controller, "Remote device failed in network provisioning\n");
        break;

    default:
        break;
    };
exit:
    if (mPairingDelegate != nullptr)
    {
        mPairingDelegate->OnStatusUpdate(status);
    }
}

void DeviceCommissioner::PersistDeviceList()
{
    if (mStorageDelegate != nullptr && mPairedDevicesUpdated)
    {
        constexpr uint16_t size = CHIP_MAX_SERIALIZED_SIZE_U64(kNumMaxPairedDevices);
        char * serialized       = static_cast<char *>(chip::Platform::MemoryAlloc(size));
        uint16_t requiredSize   = size;
        if (serialized != nullptr)
        {
            const char * value = mPairedDevices.SerializeBase64(serialized, requiredSize);
            if (value != nullptr && requiredSize <= size)
            {
                PERSISTENT_KEY_OP(static_cast<uint64_t>(0), kPairedDeviceListKeyPrefix, key,
                                  mStorageDelegate->SetKeyValue(key, value));
                mPairedDevicesUpdated = false;
            }
            chip::Platform::MemoryFree(serialized);
        }
    }
}

void DeviceCommissioner::ReleaseDevice(Device * device)
{
    PersistDeviceList();
    DeviceController::ReleaseDevice(device);
}

} // namespace Controller
} // namespace chip
