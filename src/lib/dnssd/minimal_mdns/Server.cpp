/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include "Server.h"

#include <chrono>
#include <errno.h>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include <lib/dnssd/minimal_mdns/core/DnsHeader.h>
#include <platform/CHIPDeviceLayer.h>

namespace mdns {
namespace Minimal {
namespace {

// Helper function to get current timestamp in milliseconds
inline int64_t GetCurrentTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// CSV helpers for recording packets to/from a specific IP
static std::mutex sMdnsCsvMutex;
static std::string GetMdnsCsvPath()
{
    std::string exePath;
#ifdef __linux__
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1)
    {
        buf[len] = '\0';
        exePath  = buf;
        // Get just the filename without path
        size_t pos = exePath.find_last_of('/');
        if (pos != std::string::npos)
        {
            exePath = exePath.substr(pos + 1);
        }
    }
#endif
    if (exePath.empty())
    {
        exePath = "mdns_log";
    }
    return "./" + exePath + ".csv";
}

static const std::string kMdnsCsvPath = GetMdnsCsvPath();

static void WriteMdnsCsvEntry(const char * dir, int64_t tsMs, int size, int interfacePlatform, const char * addrStr, uint16_t port)
{
    std::lock_guard<std::mutex> lock(sMdnsCsvMutex);
    std::ofstream ofs(kMdnsCsvPath, std::ios::app);
    if (!ofs)
        return;
    // CSV: DIR,TS_MS,SIZE,IF,ADDR,PORT
    ofs << dir << ',' << tsMs << ',' << size << ',' << interfacePlatform << ',' << addrStr << ',' << port << '\n';
}

class ShutdownOnError
{
public:
    ShutdownOnError(ServerBase * s) : mServer(s) {}
    ~ShutdownOnError()
    {
        if (mServer != nullptr)
        {
            mServer->Shutdown();
        }
    }

    CHIP_ERROR ReturnSuccess()
    {
        mServer = nullptr;
        return CHIP_NO_ERROR;
    }

private:
    ServerBase * mServer;
};

/**
 * Extracts the Listening UDP Endpoint from an underlying ServerBase::EndpointInfo
 */
class ListenSocketPickerDelegate : public ServerBase::BroadcastSendDelegate
{
public:
    chip::Inet::UDPEndPoint * Accept(ServerBase::EndpointInfo * info) override { return info->mListenUdp; }
};

#if CHIP_MINMDNS_USE_EPHEMERAL_UNICAST_PORT

/**
 * Extracts the Querying UDP Endpoint from an underlying ServerBase::EndpointInfo
 */
class QuerySocketPickerDelegate : public ServerBase::BroadcastSendDelegate
{
public:
    chip::Inet::UDPEndPoint * Accept(ServerBase::EndpointInfo * info) override { return info->mUnicastQueryUdp; }
};

#else

using QuerySocketPickerDelegate = ListenSocketPickerDelegate;

#endif

/**
 * Validates that an endpoint belongs to a specific interface/ip address type before forwarding the
 * endpoint accept logic to another BroadcastSendDelegate.
 *
 * Usage like:
 *
 * SomeDelegate *child = ....;
 * InterfaceTypeFilterDelegate filter(interfaceId, IPAddressType::IPv6, child);
 *
 * UDPEndPoint *udp = filter.Accept(endpointInfo);
 */
class InterfaceTypeFilterDelegate : public ServerBase::BroadcastSendDelegate
{
public:
    InterfaceTypeFilterDelegate(chip::Inet::InterfaceId interface, chip::Inet::IPAddressType type,
                                ServerBase::BroadcastSendDelegate * child) :
        mInterface(interface), mAddressType(type), mChild(child)
    {}

    chip::Inet::UDPEndPoint * Accept(ServerBase::EndpointInfo * info) override
    {
        if ((info->mInterfaceId != mInterface) && (info->mInterfaceId != chip::Inet::InterfaceId::Null()))
        {
            return nullptr;
        }

        if ((mAddressType != chip::Inet::IPAddressType::kAny) && (info->mAddressType != mAddressType))
        {
            return nullptr;
        }

        return mChild->Accept(info);
    }

private:
    chip::Inet::InterfaceId mInterface;
    chip::Inet::IPAddressType mAddressType;
    ServerBase::BroadcastSendDelegate * mChild = nullptr;
};

} // namespace

namespace BroadcastIpAddresses {

// Get standard mDNS Broadcast addresses
chip::Inet::IPAddress Get(chip::Inet::IPAddressType addressType)
{
    chip::Inet::IPAddress address;
#if INET_CONFIG_ENABLE_IPV4
    if (addressType == chip::Inet::IPAddressType::kIPv4)
    {
        VerifyOrDie(chip::Inet::IPAddress::FromString("224.0.0.251", address));
    }
    else
#endif
    {
        VerifyOrDie(chip::Inet::IPAddress::FromString("FF02::FB", address));
    }
    return address;
}

} // namespace BroadcastIpAddresses

namespace {

#if CHIP_ERROR_LOGGING
const char * AddressTypeStr(chip::Inet::IPAddressType addressType)
{
    switch (addressType)
    {
    case chip::Inet::IPAddressType::kIPv6:
        return "IPv6";
#if INET_CONFIG_ENABLE_IPV4
    case chip::Inet::IPAddressType::kIPv4:
        return "IPv4";
#endif // INET_CONFIG_ENABLE_IPV4
    default:
        return "UNKNOWN";
    }
}
#endif

} // namespace

ServerBase::~ServerBase()
{
    Shutdown();
}

void ServerBase::Shutdown()
{
    ShutdownEndpoints();
    mIsInitialized = false;
}

void ServerBase::ShutdownEndpoints()
{
    mEndpoints.ReleaseAll();
}

void ServerBase::ShutdownEndpoint(EndpointInfo & aEndpoint)
{
    mEndpoints.ReleaseObject(&aEndpoint);
}

bool ServerBase::IsListening() const
{
    bool listening = false;
    mEndpoints.ForEachActiveObject([&](auto * endpoint) {
        if (endpoint->mListenUdp != nullptr)
        {
            listening = true;
            return chip::Loop::Break;
        }
        return chip::Loop::Continue;
    });
    return listening;
}

CHIP_ERROR ServerBase::Listen(chip::Inet::EndPointManager<chip::Inet::UDPEndPoint> * udpEndPointManager, ListenIterator * it,
                              uint16_t port)
{
    ShutdownEndpoints(); // ensure everything starts fresh

    chip::Inet::InterfaceId interfaceId = chip::Inet::InterfaceId::Null();
    chip::Inet::IPAddressType addressType;

    ShutdownOnError autoShutdown(this);

    while (it->Next(&interfaceId, &addressType))
    {
        chip::Inet::UDPEndPoint * listenUdp;
        ReturnErrorOnFailure(udpEndPointManager->NewEndPoint(&listenUdp));
        std::unique_ptr<chip::Inet::UDPEndPoint, EndpointInfo::EndPointDeletor> endPointHolder(listenUdp, {});

        ReturnErrorOnFailure(listenUdp->Bind(addressType, chip::Inet::IPAddress::Any, port, interfaceId));

        ReturnErrorOnFailure(listenUdp->Listen(OnUdpPacketReceived, nullptr /*OnReceiveError*/, this));

        CHIP_ERROR err = listenUdp->JoinMulticastGroup(interfaceId, BroadcastIpAddresses::Get(addressType));

        if (err != CHIP_NO_ERROR)
        {
            char interfaceName[chip::Inet::InterfaceId::kMaxIfNameLength];
            interfaceId.GetInterfaceName(interfaceName, sizeof(interfaceName));

            // Log only as non-fatal error. Failure to join will mean we reply to unicast queries only.
            ChipLogError(DeviceLayer, "MDNS failed to join multicast group on %s for address type %s: %" CHIP_ERROR_FORMAT,
                         interfaceName, AddressTypeStr(addressType), err.Format());

            endPointHolder.reset();
        }

#if CHIP_MINMDNS_USE_EPHEMERAL_UNICAST_PORT
        // Separate UDP endpoint for unicast queries, bound to 0 (i.e. pick random ephemeral port)
        //   - helps in not having conflicts on port 5353, will receive unicast replies directly
        //   - has a *DRAWBACK* of unicast queries being considered LEGACY by mdns since they do
        //     not originate from 5353 and the answers will include a query section.
        chip::Inet::UDPEndPoint * unicastQueryUdp;
        ReturnErrorOnFailure(udpEndPointManager->NewEndPoint(&unicastQueryUdp));
        std::unique_ptr<chip::Inet::UDPEndPoint, EndpointInfo::EndPointDeletor> endPointHolderUnicast(unicastQueryUdp, {});
        ReturnErrorOnFailure(unicastQueryUdp->Bind(addressType, chip::Inet::IPAddress::Any, 0, interfaceId));
        ReturnErrorOnFailure(unicastQueryUdp->Listen(OnUdpPacketReceived, nullptr /*OnReceiveError*/, this));
#endif

#if CHIP_MINMDNS_USE_EPHEMERAL_UNICAST_PORT
        if (endPointHolder || endPointHolderUnicast)
        {
            // If allocation fails, the rref will not be consumed, so that the endpoint will also be freed correctly
            mEndpoints.CreateObject(interfaceId, addressType, std::move(endPointHolder), std::move(endPointHolderUnicast));
        }
#else
        if (endPointHolder)
        {
            // If allocation fails, the rref will not be consumed, so that the endpoint will also be freed correctly
            mEndpoints.CreateObject(interfaceId, addressType, std::move(endPointHolder));
        }
#endif

        // If at least one IPv6 interface is used by the mDNS server, notify the application that DNS-SD is ready.
        if (!mIsInitialized && addressType == chip::Inet::IPAddressType::kIPv6)
        {
#if !CHIP_DEVICE_LAYER_NONE
            chip::DeviceLayer::ChipDeviceEvent event{};
            event.Type = chip::DeviceLayer::DeviceEventType::kDnssdInitialized;
            chip::DeviceLayer::PlatformMgr().PostEventOrDie(&event);
#endif
            mIsInitialized = true;
        }
    }

    return autoShutdown.ReturnSuccess();
}

CHIP_ERROR ServerBase::DirectSend(chip::System::PacketBufferHandle && data, const chip::Inet::IPAddress & addr, uint16_t port,
                                  chip::Inet::InterfaceId interface)
{
    CHIP_ERROR err = CHIP_ERROR_NOT_CONNECTED;
    mEndpoints.ForEachActiveObject([&](auto * info) {
        if (info->mListenUdp == nullptr)
        {
            return chip::Loop::Continue;
        }

        if (info->mAddressType != addr.Type())
        {
            return chip::Loop::Continue;
        }

        chip::Inet::InterfaceId boundIf = info->mListenUdp->GetBoundInterface();

        if ((boundIf.IsPresent()) && (boundIf != interface))
        {
            return chip::Loop::Continue;
        }

        // capture size before moving the buffer into SendTo
        int pktSize = static_cast<int>(data->DataLength());
        err         = info->mListenUdp->SendTo(addr, port, std::move(data));

        if (err == CHIP_NO_ERROR)
        {
            char addrStr[INET6_ADDRSTRLEN];
            addr.ToString(addrStr, sizeof(addrStr));
            ChipLogProgress(Discovery, "[MINIMAL_MDNS_TIMING] UNICAST_SEND: to=%s:%u, interface=%d, time=%lld", addrStr, port,
                            interface.GetPlatformInterface(), static_cast<long long>(GetCurrentTimeMs()));
            // If we're sending to the target IP, record CSV entry
            if (strncmp(addrStr, "fe80::9e6b:ff:fe2d:feb4", 23) == 0 || strncmp(addrStr, "192.168.1.55", 13) == 0)
            {
                WriteMdnsCsvEntry("SEND", GetCurrentTimeMs(), pktSize, interface.GetPlatformInterface(), addrStr, port);
            }
        }

        return chip::Loop::Break;
    });

    return err;
}

CHIP_ERROR ServerBase::BroadcastUnicastQuery(chip::System::PacketBufferHandle && data, uint16_t port)
{
    QuerySocketPickerDelegate socketPicker;
    return BroadcastImpl(std::move(data), port, &socketPicker);
}

CHIP_ERROR ServerBase::BroadcastUnicastQuery(chip::System::PacketBufferHandle && data, uint16_t port,
                                             chip::Inet::InterfaceId interface, chip::Inet::IPAddressType addressType)
{
    QuerySocketPickerDelegate socketPicker;
    InterfaceTypeFilterDelegate filter(interface, addressType, &socketPicker);

    return BroadcastImpl(std::move(data), port, &filter);
}

CHIP_ERROR ServerBase::BroadcastSend(chip::System::PacketBufferHandle && data, uint16_t port, chip::Inet::InterfaceId interface,
                                     chip::Inet::IPAddressType addressType)
{
    ListenSocketPickerDelegate socketPicker;
    InterfaceTypeFilterDelegate filter(interface, addressType, &socketPicker);

    return BroadcastImpl(std::move(data), port, &filter);
}

CHIP_ERROR ServerBase::BroadcastSend(chip::System::PacketBufferHandle && data, uint16_t port)
{
    ListenSocketPickerDelegate socketPicker;
    return BroadcastImpl(std::move(data), port, &socketPicker);
}

CHIP_ERROR ServerBase::BroadcastImpl(chip::System::PacketBufferHandle && data, uint16_t port, BroadcastSendDelegate * delegate)
{
    int64_t sendTime = GetCurrentTimeMs();
    ChipLogProgress(Discovery, "[MINIMAL_MDNS_TIMING] BROADCAST_SEND: size=%d, port=%u, time=%lld",
                    static_cast<int>(data->DataLength()), port, static_cast<long long>(sendTime));

    // Broadcast requires sending data multiple times, each of which may error
    // out, yet broadcast only has a single error code.
    //
    // The general logic of error handling is:
    //   - if no send done at all, return error
    //   - if at least one broadcast succeeds, assume success overall
    //   + some internal consistency validations for state error.

    unsigned successes   = 0;
    unsigned failures    = 0;
    CHIP_ERROR lastError = CHIP_ERROR_NO_ENDPOINT;

    if (chip::Loop::Break == mEndpoints.ForEachActiveObject([&](auto * info) {
            chip::Inet::UDPEndPoint * udp = delegate->Accept(info);

            if (udp == nullptr)
            {
                return chip::Loop::Continue;
            }

            CHIP_ERROR err = CHIP_NO_ERROR;

            /// The same packet needs to be sent over potentially multiple interfaces.
            /// LWIP does not like having a pbuf sent over serparate interfaces, hence we create a copy
            /// for sending via `CloneData`
            ///
            /// TODO: this wastes one copy of the data and that could be optimized away
            chip::System::PacketBufferHandle tempBuf = data.CloneData();
            if (tempBuf.IsNull())
            {
                // Not enough memory available to clone pbuf
                err = CHIP_ERROR_NO_MEMORY;
            }
            else if (info->mAddressType == chip::Inet::IPAddressType::kIPv6)
            {
                err = udp->SendTo(mIpv6BroadcastAddress, port, std::move(tempBuf), udp->GetBoundInterface());
            }
#if INET_CONFIG_ENABLE_IPV4
            else if (info->mAddressType == chip::Inet::IPAddressType::kIPv4)
            {
                err = udp->SendTo(mIpv4BroadcastAddress, port, std::move(tempBuf), udp->GetBoundInterface());
            }
#endif
            else
            {
                // This is a general error of internal consistency: every address has a known type. Fail completely otherwise.
                lastError = CHIP_ERROR_INCORRECT_STATE;
                return chip::Loop::Break;
            }

            if (err == CHIP_NO_ERROR)
            {
                successes++;
            }
            else
            {
                failures++;
                lastError = err;
#if CHIP_DETAIL_LOGGING
                char ifaceName[chip::Inet::InterfaceId::kMaxIfNameLength];
                err = info->mInterfaceId.GetInterfaceName(ifaceName, sizeof(ifaceName));
                if (err != CHIP_NO_ERROR)
                    strcpy(ifaceName, "???");
                ChipLogDetail(Discovery, "Warning: Attempt to mDNS broadcast failed on %s:  %s", ifaceName, lastError.AsString());
#endif
            }
            return chip::Loop::Continue;
        }))
    {
        return lastError;
    }

    if (failures != 0)
    {
        // if we had failures, log if the final status was success or failure, to make log reading
        // easier. Some mDNS failures may be expected (e.g. for interfaces unavailable)
        if (successes != 0)
        {
            ChipLogDetail(Discovery, "mDNS broadcast had only partial success: %u successes and %u failures.", successes, failures);
        }
        else
        {
            ChipLogProgress(Discovery, "mDNS broadcast full failed in %u separate send attempts.", failures);
        }
    }

    if (!successes)
    {
        return lastError;
    }

    return CHIP_NO_ERROR;
}

void ServerBase::OnUdpPacketReceived(chip::Inet::UDPEndPoint * endPoint, chip::System::PacketBufferHandle && buffer,
                                     const chip::Inet::IPPacketInfo * info)
{
    int64_t receiveTime = GetCurrentTimeMs();

    ServerBase * srv = static_cast<ServerBase *>(endPoint->mAppState);
    if (!srv->mDelegate)
    {
        return;
    }

    mdns::Minimal::BytesRange data(buffer->Start(), buffer->Start() + buffer->DataLength());
    if (data.Size() < HeaderRef::kSizeBytes)
    {
        ChipLogError(Discovery, "Packet to small for mDNS data: %d bytes", static_cast<int>(data.Size()));
        return;
    }

    if (HeaderRef(const_cast<uint8_t *>(data.Start())).GetFlags().IsQuery())
    {
        // Only consider queries that are received on the same interface we are listening on.
        // Without this, queries show up on all addresses on all interfaces, resulting
        // in more replies than one would expect.
        if (endPoint->GetBoundInterface() == info->Interface)
        {
            // Only log and process Matter-related queries
            // Check for Matter service types: _matter._tcp, _matterc._udp, or _chip._tcp (legacy)
            bool isMatterQuery         = false;
            const uint8_t * packetData = data.Start();
            size_t packetSize          = data.Size();

            for (size_t i = 0; i + 7 < packetSize; i++)
            {
                if (memcmp(&packetData[i], "_matter", 7) == 0 || (i + 5 < packetSize && memcmp(&packetData[i], "_chip", 5) == 0))
                {
                    isMatterQuery = true;
                    break;
                }
            }

            // Only process Matter-related queries - ignore all other mDNS traffic
            if (isMatterQuery)
            {
                char addrStr[INET6_ADDRSTRLEN];
                info->SrcAddress.ToString(addrStr, sizeof(addrStr));
                ChipLogProgress(Discovery, "[MINIMAL_MDNS_TIMING] MATTER_QUERY_RECV: size=%d, interface=%d, from=%s:%u, time=%lld",
                                static_cast<int>(data.Size()), info->Interface.GetPlatformInterface(), addrStr, info->SrcPort,
                                static_cast<long long>(receiveTime));
                // If packet is from the target IP, write CSV entry
                if (strncmp(addrStr, "fe80::9e6b:ff:fe2d:feb4", 23) == 0 || strncmp(addrStr, "192.168.1.55", 13) == 0)

                {
                    WriteMdnsCsvEntry("RECVQuery", receiveTime, static_cast<int>(data.Size()),
                                      info->Interface.GetPlatformInterface(), addrStr, info->SrcPort);
                }
                srv->mDelegate->OnQuery(data, info);
            }
        }
    }
    else
    {
        // Only log and process Matter-related responses
        bool isMatterResponse      = false;
        const uint8_t * packetData = data.Start();
        size_t packetSize          = data.Size();

        for (size_t i = 0; i + 7 < packetSize; i++)
        {
            if (memcmp(&packetData[i], "_matter", 7) == 0 || (i + 5 < packetSize && memcmp(&packetData[i], "_chip", 5) == 0))
            {
                isMatterResponse = true;
                break;
            }
        }

        // Only process Matter-related responses - ignore all other mDNS traffic
        if (isMatterResponse)
        {
            char addrStr[INET6_ADDRSTRLEN];
            info->SrcAddress.ToString(addrStr, sizeof(addrStr));
            ChipLogProgress(Discovery, "[MINIMAL_MDNS_TIMING] MATTER_RESPONSE_RECV: size=%d, interface=%d, from=%s:%u, time=%lld",
                            static_cast<int>(data.Size()), info->Interface.GetPlatformInterface(), addrStr, info->SrcPort,
                            static_cast<long long>(receiveTime));
            // If packet is from the target IP, write CSV entry
            if (strncmp(addrStr, "fe80::9e6b:ff:fe2d:feb4", 23) == 0 || strncmp(addrStr, "192.168.1.55", 13) == 0)
            {
                WriteMdnsCsvEntry("RECVResponse", receiveTime, static_cast<int>(data.Size()),
                                  info->Interface.GetPlatformInterface(), addrStr, info->SrcPort);
            }
            srv->mDelegate->OnResponse(data, info);
        }
    }
}

} // namespace Minimal
} // namespace mdns
