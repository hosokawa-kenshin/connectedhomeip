#ifndef SRC_APP_CLUSTERS_LOCATION_DETECTOR_CLUSTER_SERVER_SERVER_H_
#define SRC_APP_CLUSTERS_LOCATION_DETECTOR_CLUSTER_SERVER_SERVER_H_

#include <app-common/zap-generated/cluster-objects.h>
#include <app/AttributeAccessInterface.h>
#include <app/CommandHandlerInterface.h>
#include <app/ConcreteCommandPath.h>
#include <app/util/af-types.h>
#include <app/util/basic-types.h>
#include <app/util/config.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceConfig.h>

#ifdef ZCL_USING_LOCATION_DETECTOR_CLUSTER_SERVER
#define LOCATION_DETECTOR_NUM_SUPPORTED_ENDPOINTS                                                                                  \
    (MATTER_DM_LOCATION_DETECTOR_CLUSTER_SERVER_ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
#else
#define LOCATION_DETECTOR_NUM_SUPPORTED_ENDPOINTS CHIP_DEVICE_CONFIG_ENDPOINT_COUNT_LOCATION_DETECTOR
#endif /* ZCL_USING_LOCATION_DETECTOR_CLUSTER_SERVER */
static constexpr size_t kNumSupportedEndpoints = LOCATION_DETECTOR_NUM_SUPPORTED_ENDPOINTS;

char * getTimestamp();
std::string getDBPath();

namespace chip {
namespace app {
namespace Clusters {
namespace LocationDetector {

class LocationDetectorContent
{
public:
    EndpointId endpoint;

    // Attribute List
    uint16_t distance;
    char uidChar[37];
    char MediatorUID[20];
    char log[60];

    LocationDetectorContent(EndpointId endpoint);
    LocationDetectorContent();
};

class LocationDetectorServer : public AttributeAccessInterface, public CommandHandlerInterface
{
public:
    // Register on all endpoints.
    LocationDetectorServer() :
        AttributeAccessInterface(Optional<EndpointId>::Missing(), LocationDetector::Id),
        CommandHandlerInterface(Optional<EndpointId>(), Id)
    {}
    static LocationDetectorServer & Instance();

    // Currently not used, but should be called from a whole-cluster shutdown
    // callback once cluster lifecycle is clearer
    void Shutdown();

    // // Attributes
    CHIP_ERROR Read(const ConcreteReadAttributePath & aPath, AttributeValueEncoder & aEncoder) override;
    CHIP_ERROR Write(const ConcreteDataAttributePath & aPath, AttributeValueDecoder & aDecoder) override;
    void InvokeCommand(HandlerContext & ctx) override;

    // Attribute storage
#if LOCATION_DETECTOR_NUM_SUPPORTED_ENDPOINTS > 0
    LocationDetectorContent content[kNumSupportedEndpoints];
#else
    LocationDetectorContent * content = nullptr;
#endif

    size_t GetNumSupportedEndpoints() const;
    CHIP_ERROR RegisterEndpoint(EndpointId endpointId);
    CHIP_ERROR UnregisterEndpoint(EndpointId endpointId);

private:
    // both return std::numeric_limits<size_t>::max() for not found
    size_t EndpointIndex(EndpointId endpointId) const;
    size_t NextEmptyIndex() const;
};
} // namespace LocationDetector
} // namespace Clusters
} // namespace app
} // namespace chip

#endif
