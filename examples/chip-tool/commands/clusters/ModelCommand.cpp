/*
 *   Copyright (c) 2020 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include "ModelCommand.h"

#include <app/InteractionModelEngine.h>
#include <app/icd/client/DefaultICDClientStorage.h>
#include <chrono>
#include <fstream>
#include <inttypes.h>

using namespace ::chip;

// External reference to global mDNS resolution complete timestamp from OperationalSessionSetup.cpp
namespace chip {
extern std::chrono::high_resolution_clock::time_point g_lastMdnsResolutionCompleteTime;
// CASE timing globals (defined here in chip-tool)
std::chrono::high_resolution_clock::time_point g_caseSigma1SentTime;
std::chrono::high_resolution_clock::time_point g_caseSigma2ReceivedTime;
std::chrono::high_resolution_clock::time_point g_caseSigma3SendingTime;
} // namespace chip

// Helper function to write CASE timing to CSV
static void WriteCaseTimingToCsv()
{
    const char * csvPath = "chip_tool_case_timing.csv";

    // Use epoch seconds (整数秒) instead of milliseconds
    auto sigma1Sec = std::chrono::duration_cast<std::chrono::seconds>(chip::g_caseSigma1SentTime.time_since_epoch()).count();
    auto sigma2Sec = std::chrono::duration_cast<std::chrono::seconds>(chip::g_caseSigma2ReceivedTime.time_since_epoch()).count();
    auto sigma3Sec = std::chrono::duration_cast<std::chrono::seconds>(chip::g_caseSigma3SendingTime.time_since_epoch()).count();

    // Check if file exists
    bool fileExists = false;
    std::ifstream checkFile(csvPath);
    if (checkFile.good())
    {
        fileExists = true;
    }
    checkFile.close();

    std::ofstream csvFile(csvPath, std::ios::app);
    if (!csvFile.is_open())
    {
        return;
    }

    if (!fileExists)
    {
        csvFile << "Sigma1_Sent_s,Sigma2_Received_s,Sigma3_Sending_s" << std::endl;
    }

    csvFile << sigma1Sec << "," << sigma2Sec << "," << sigma3Sec << std::endl;
    csvFile.close();
}

CHIP_ERROR ModelCommand::RunCommand()
{
    // Record command start time
    mCommandStartTime = std::chrono::high_resolution_clock::now();

    if (IsGroupId(mDestinationId))
    {
        FabricIndex fabricIndex = CurrentCommissioner().GetFabricIndex();
        ChipLogProgress(chipTool, "Sending command to group 0x%x", GroupIdFromNodeId(mDestinationId));

        return SendGroupCommand(GroupIdFromNodeId(mDestinationId), fabricIndex);
    }

    ChipLogProgress(chipTool, "Sending command to node 0x%" PRIx64, mDestinationId);
    CheckPeerICDType();

    CommissioneeDeviceProxy * commissioneeDeviceProxy = nullptr;
    if (CHIP_NO_ERROR == CurrentCommissioner().GetDeviceBeingCommissioned(mDestinationId, &commissioneeDeviceProxy))
    {
        // Using device being commissioned (no CASE needed)
        mGetConnectedDeviceCallTime = std::chrono::high_resolution_clock::now();
        mDeviceConnectedTime        = mGetConnectedDeviceCallTime;
        mSendCommandCallTime        = mDeviceConnectedTime;
        return SendCommand(commissioneeDeviceProxy, mEndPointId);
    }

    // Check whether the session needs to allow large payload support.
    TransportPayloadCapability transportPayloadCapability =
        AllowLargePayload() ? TransportPayloadCapability::kLargePayload : TransportPayloadCapability::kMRPPayload;

    // Record GetConnectedDevice call time (start of mDNS + CASE)
    mGetConnectedDeviceCallTime = std::chrono::high_resolution_clock::now();

    ChipLogProgress(chipTool, "Requesting device connection (mDNS + CASE session)...");

    return CurrentCommissioner().GetConnectedDevice(mDestinationId, &mOnDeviceConnectedCallback,
                                                    &mOnDeviceConnectionFailureCallback, transportPayloadCapability);
}

void ModelCommand::OnDeviceConnectedFn(void * context, chip::Messaging::ExchangeManager & exchangeMgr,
                                       const chip::SessionHandle & sessionHandle)
{
    ModelCommand * command = reinterpret_cast<ModelCommand *>(context);
    VerifyOrReturn(command != nullptr, ChipLogError(chipTool, "OnDeviceConnectedFn: context is null"));

    // Capture mDNS resolution complete time from global variable
    command->mMdnsDiscoveryTime = chip::g_lastMdnsResolutionCompleteTime;

    // Debug: Check if mDNS timestamp was captured
    auto mdnsMs = std::chrono::duration_cast<std::chrono::milliseconds>(command->mMdnsDiscoveryTime.time_since_epoch()).count();
    ChipLogProgress(chipTool, "mDNS resolution complete timestamp captured: %lld ms (0 means no resolution)",
                    static_cast<long long>(mdnsMs));

    // Record device connected time (mDNS + TCP/UDP + CASE session complete)
    command->mDeviceConnectedTime = std::chrono::high_resolution_clock::now();
    ChipLogProgress(chipTool, "Device connected (CASE session established)");

    // Write CASE timing to CSV
    WriteCaseTimingToCsv();

    chip::OperationalDeviceProxy device(&exchangeMgr, sessionHandle);

    // Record command send call time
    command->mSendCommandCallTime = std::chrono::high_resolution_clock::now();

    CHIP_ERROR err = command->SendCommand(&device, command->mEndPointId);
    VerifyOrReturn(CHIP_NO_ERROR == err, command->SetCommandExitStatus(err));
}

void ModelCommand::OnDeviceConnectionFailureFn(void * context, const chip::ScopedNodeId & peerId, CHIP_ERROR err)
{
    LogErrorOnFailure(err);

    ModelCommand * command = reinterpret_cast<ModelCommand *>(context);
    VerifyOrReturn(command != nullptr, ChipLogError(chipTool, "OnDeviceConnectionFailureFn: context is null"));
    command->SetCommandExitStatus(err);
}

void ModelCommand::Shutdown()
{
    mOnDeviceConnectedCallback.Cancel();
    mOnDeviceConnectionFailureCallback.Cancel();

    // Log detailed timing if we have recorded timestamps
    if (mCommandStartTime.time_since_epoch().count() > 0 && mCommandCompleteTime.time_since_epoch().count() > 0)
    {
        LogDetailedTiming();
    }

    CHIPCommand::Shutdown();
}

void ModelCommand::ClearICDEntry(const ScopedNodeId & nodeId)
{
    CHIP_ERROR deleteEntryError = CHIPCommand::sICDClientStorage.DeleteEntry(nodeId);
    if (deleteEntryError != CHIP_NO_ERROR)
    {
        ChipLogError(chipTool, "Failed to delete ICD entry: %" CHIP_ERROR_FORMAT, deleteEntryError.Format());
    }
}

void ModelCommand::StoreICDEntryWithKey(app::ICDClientInfo & clientInfo, ByteSpan key)
{
    CHIP_ERROR err = CHIPCommand::sICDClientStorage.SetKey(clientInfo, key);
    if (err == CHIP_NO_ERROR)
    {
        err = CHIPCommand::sICDClientStorage.StoreEntry(clientInfo);
    }

    if (err != CHIP_NO_ERROR)
    {
        CHIPCommand::sICDClientStorage.RemoveKey(clientInfo);
        ChipLogError(chipTool, "Failed to persist symmetric key with error: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
}

void ModelCommand::CheckPeerICDType()
{
    if (mIsPeerLIT.HasValue())
    {
        ChipLogProgress(chipTool, "Peer ICD type is set to %s", mIsPeerLIT.Value() == 1 ? "LIT-ICD" : "non LIT-ICD");
        return;
    }

    app::ICDClientInfo info;
    auto destinationPeerId = chip::ScopedNodeId(mDestinationId, CurrentCommissioner().GetFabricIndex());
    auto iter              = CHIPCommand::sICDClientStorage.IterateICDClientInfo();
    if (iter == nullptr)
    {
        return;
    }
    app::DefaultICDClientStorage::ICDClientInfoIteratorWrapper clientInfoIteratorWrapper(iter);

    while (iter->Next(info))
    {
        if (ScopedNodeId(info.peer_node.GetNodeId(), info.peer_node.GetFabricIndex()) == destinationPeerId)
        {
            ChipLogProgress(chipTool, "Peer is a registered LIT ICD.");
            mIsPeerLIT.SetValue(true);
            return;
        }
    }
}

bool ModelCommand::IsPeerLIT()
{
    CheckPeerICDType();
    return mIsPeerLIT.ValueOr(false);
}

bool ModelCommand::AllowLargePayload()
{
    return mAllowLargePayload.ValueOr(false);
}

void ModelCommand::LogDetailedTiming()
{
    // Static counter for entry numbers
    static long long entryNumber = -1;

    // Initialize entry number from existing CSV file on first call
    if (entryNumber == -1)
    {
        entryNumber = 0;
        std::ifstream existingFile("chip_tool_detailed_timing.csv");
        if (existingFile.good())
        {
            std::string line;
            long long maxId  = 0;
            bool isFirstLine = true;

            while (std::getline(existingFile, line))
            {
                if (isFirstLine)
                {
                    isFirstLine = false;
                    continue; // Skip header
                }

                // Parse the first column (ID)
                size_t commaPos = line.find(',');
                if (commaPos != std::string::npos)
                {
                    std::string idStr = line.substr(0, commaPos);
                    // Simple integer parsing without exceptions
                    char * endPtr = nullptr;
                    long long id  = strtoll(idStr.c_str(), &endPtr, 10);
                    // Check if parsing was successful
                    if (endPtr != idStr.c_str() && id > maxId)
                    {
                        maxId = id;
                    }
                }
            }
            entryNumber = maxId;
        }
        existingFile.close();
    }

    entryNumber++;

    // Calculate absolute timestamps in milliseconds since epoch (define these first)
    auto mdnsDiscoveryMs = std::chrono::duration_cast<std::chrono::milliseconds>(mMdnsDiscoveryTime.time_since_epoch()).count();
    auto getConnectedDeviceMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(mGetConnectedDeviceCallTime.time_since_epoch()).count();
    auto deviceConnectedMs = std::chrono::duration_cast<std::chrono::milliseconds>(mDeviceConnectedTime.time_since_epoch()).count();
    auto sendCommandCallMs = std::chrono::duration_cast<std::chrono::milliseconds>(mSendCommandCallTime.time_since_epoch()).count();
    auto responseReceivedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(mResponseReceivedTime.time_since_epoch()).count();
    auto commandCompleteMs = std::chrono::duration_cast<std::chrono::milliseconds>(mCommandCompleteTime.time_since_epoch()).count();

    // Calculate durations in milliseconds
    auto mdnsCaseMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(mDeviceConnectedTime - mGetConnectedDeviceCallTime).count();
    auto mdnsDiscoveryDurationMs = (mdnsDiscoveryMs > 0) ? (mdnsDiscoveryMs - getConnectedDeviceMs) : 0;
    auto caseDurationMs          = (mdnsDiscoveryMs > 0) ? (deviceConnectedMs - mdnsDiscoveryMs) : mdnsCaseMs;
    auto commandPrepMs = std::chrono::duration_cast<std::chrono::milliseconds>(mSendCommandCallTime - mDeviceConnectedTime).count();
    auto commandSendMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(mResponseReceivedTime - mSendCommandCallTime).count();
    auto responseProcessMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(mCommandCompleteTime - mResponseReceivedTime).count();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(mCommandCompleteTime - mCommandStartTime).count();

    ChipLogProgress(chipTool, "=== Detailed Timing (Entry #%lld) ===", entryNumber);
    ChipLogProgress(chipTool, "GetConnectedDevice Start: %lld ms (epoch)", static_cast<long long>(getConnectedDeviceMs));
    ChipLogProgress(chipTool, "mDNS Discovery Complete: %lld ms (epoch)", static_cast<long long>(mdnsDiscoveryMs));
    ChipLogProgress(chipTool, "mDNS Discovery Duration: %lld ms", static_cast<long long>(mdnsDiscoveryDurationMs));
    ChipLogProgress(chipTool, "CASE Duration: %lld ms", static_cast<long long>(caseDurationMs));
    ChipLogProgress(chipTool, "Total Connection (mDNS + CASE): %lld ms", static_cast<long long>(mdnsCaseMs));
    ChipLogProgress(chipTool, "Command Preparation: %lld ms", static_cast<long long>(commandPrepMs));
    ChipLogProgress(chipTool, "Command Send + Response: %lld ms", static_cast<long long>(commandSendMs));
    ChipLogProgress(chipTool, "Response Processing: %lld ms", static_cast<long long>(responseProcessMs));
    ChipLogProgress(chipTool, "Total: %lld ms", static_cast<long long>(totalMs));

    // Write to CSV
    std::ofstream csvFile;
    const char * csvFilePath = "chip_tool_detailed_timing.csv";

    bool fileExists = false;
    std::ifstream checkFile(csvFilePath);
    if (checkFile.good())
    {
        fileExists = true;
    }
    checkFile.close();

    csvFile.open(csvFilePath, std::ios::app);
    if (!csvFile.is_open())
    {
        ChipLogError(chipTool, "Failed to open detailed timing CSV file");
        return;
    }

    if (!fileExists)
    {
        csvFile << "ID,GetConnectedDevice_Start_ms,mDNS_Discovery_ms,DeviceConnected_End_ms,SendCommand_Start_ms,Response_Received_"
                   "ms,Command_Complete_ms"
                << std::endl;
    }

    csvFile << entryNumber << "," << getConnectedDeviceMs << "," << mdnsDiscoveryMs << "," << deviceConnectedMs << ","
            << sendCommandCallMs << "," << responseReceivedMs << "," << commandCompleteMs << std::endl;
    csvFile.close();

    ChipLogProgress(chipTool, "Detailed timing data saved to %s", csvFilePath);
}
