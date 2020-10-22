#ifndef CORE_PARAMS_H_
#define CORE_PARAMS_H_

#include <string>

#include "core/listeners.h"
#include "platform/base/byte_array.h"

namespace location {
namespace nearby {
namespace connections {

// Used by Discovery in Core::RequestConnection().
// Used by Advertising in Core::StartAdvertising().
struct ConnectionRequestInfo {
  // endpoint_info - Identifing information about this endpoint (eg. name,
  //                 device type).
  // listener      - A set of callbacks notified when remote endpoints request a
  //                 connection to this endpoint.
  ByteArray endpoint_info;
  ConnectionListener listener;
};

}  // namespace connections
}  // namespace nearby
}  // namespace location

#endif  // CORE_PARAMS_H_
