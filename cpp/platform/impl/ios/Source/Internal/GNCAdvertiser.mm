// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#import "third_party/nearby/cpp/platform/impl/ios/Source/GNCAdvertiser.h"

#include <string>

#include "third_party/absl/functional/bind_front.h"
#include "third_party/nearby/cpp/core/core.h"
#include "third_party/nearby/cpp/core/listeners.h"
#include "third_party/nearby/cpp/core/options.h"
#include "third_party/nearby/cpp/core/params.h"
#include "third_party/nearby/cpp/core/status.h"
#include "third_party/nearby/cpp/platform/base/byte_array.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/GNCConnection.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/Internal/GNCCore.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/Internal/GNCCoreConnection.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/Internal/GNCPayloadListener.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/Internal/GNCUtils.h"
#import "third_party/nearby/cpp/platform/impl/ios/Source/Platform/utils.h"
#import "third_party/objective_c/google_toolbox_for_mac/Foundation/GTMLogger.h"

NS_ASSUME_NONNULL_BEGIN

using ::location::nearby::ByteArrayFromNSData;
using ::location::nearby::CppStringFromObjCString;
using ::location::nearby::ObjCStringFromCppString;
using ::location::nearby::connections::ConnectionListener;
using ::location::nearby::connections::ConnectionOptions;
using ::location::nearby::connections::ConnectionRequestInfo;
using ::location::nearby::connections::ConnectionResponseInfo;
using ::location::nearby::connections::GNCStrategyToStrategy;
using ::location::nearby::connections::Medium;
using ResultListener = ::location::nearby::connections::ResultCallback;
using ::location::nearby::connections::Status;

/** This is a GNCAdvertiserConnectionInfo that provides storage for its properties. */
@interface GNCAdvertiserConnectionInfo : NSObject

@property(nonatomic, readonly) NSString *name;
@property(nonatomic, readonly) NSString *authToken;

- (instancetype)initWithName:(NSString *)name authToken:(NSString *)authToken;

@end

@implementation GNCAdvertiserConnectionInfo

- (instancetype)initWithName:(NSString *)name authToken:(NSString *)authToken {
  self = [super init];
  if (self) {
    _name = [name copy];
    _authToken = [authToken copy];
  }
  return self;
}

@end

/** Information retained about an endpoint before and after requesting a connection. */
@interface GNCAdvertiserEndpointInfo : NSObject
@property(nonatomic) GNCAdvertiserConnectionInfo *connectionInfo;
@property(nonatomic) GNCConnectionResponse clientResponse;
@property(nonatomic) BOOL clientResponseReceived;  // whether the client response has been received
@property(nonatomic, nullable) GNCConnectionResultHandlers *connectionResultHandlers;
@property(nonatomic, weak) GNCCoreConnection *connection;
@property(nonatomic) GNCConnectionHandlers *connectionHandlers;
@end

@implementation GNCAdvertiserEndpointInfo

+ (instancetype)infoWithEndpointConnectionInfo:(GNCAdvertiserConnectionInfo *)connInfo {
  GNCAdvertiserEndpointInfo *info = [[GNCAdvertiserEndpointInfo alloc] init];
  info.connectionInfo = connInfo;
  return info;
}

@end

/** GNCAdvertiser members. */
@interface GNCAdvertiser ()
@property(nonatomic) GNCCore *core;
@property(nonatomic) GNCAdvertiserConnectionInitiationHandler initiationHandler;
@property(nonatomic, assign) Status status;
@property(nonatomic) NSMutableDictionary<GNCEndpointId, GNCAdvertiserEndpointInfo *> *endpoints;
@end

/** C++ classes passed to the core library by GNCAdvertiser. */
namespace location {
namespace nearby {
namespace connections {

/** This class contains the callbacks for establishing and severing a connection. */
class GNCAdvertiserConnectionListener {
 public:
  explicit GNCAdvertiserConnectionListener(GNCAdvertiser *advertiser) : advertiser_(advertiser) {}

  void OnInitiated(const std::string &endpoint_id, const ConnectionResponseInfo &info) {
    GNCAdvertiser *advertiser = advertiser_;  // strongify
    if (!advertiser) return;

    NSString *endpointId = ObjCStringFromCppString(endpoint_id);
    GNCAdvertiserEndpointInfo *endpointInfo = advertiser.endpoints[endpointId];
    if (endpointInfo) {
      GTMLoggerError(@"Connection already initiated for endpoint: %@", endpointId);
    } else {
      // TODO(b/169292092): endpointInfo is an advertisement byte array. Need to implement to
      // extract the endpoint name not just force to cast string.
      NSString *name = ObjCStringFromCppString(std::string(info.remote_endpoint_info));
      NSString *authToken = ObjCStringFromCppString(info.authentication_token);
      GNCAdvertiserConnectionInfo *connInfo =
          [[GNCAdvertiserConnectionInfo alloc] initWithName:name authToken:authToken];
      endpointInfo = [GNCAdvertiserEndpointInfo infoWithEndpointConnectionInfo:connInfo];

      // Call the connection initiation handler. Synchronous because it returns the connection
      // result handlers.
      dispatch_sync(dispatch_get_main_queue(), ^{
        __weak __typeof__(advertiser) weakAdvertiser = advertiser;
        endpointInfo.connectionResultHandlers = advertiser.initiationHandler(
            endpointId, (id<GNCAdvertiserConnectionInfo>)connInfo,
            ^(GNCConnectionResponse response) {
              __strong __typeof__(advertiser) strongAdvertiser = weakAdvertiser;
              endpointInfo.clientResponse = response;
              endpointInfo.clientResponseReceived = YES;
              if (response == GNCConnectionResponseAccept) {
                // The connection was accepted by the client.
                if (payload_listener_ == nullptr) {
                  payload_listener_ = std::make_unique<GNCPayloadListener>(
                      advertiser.core,
                      ^{
                        return endpointInfo.connectionHandlers;
                      },
                      ^{
                        return endpointInfo.connection.payloads;
                      });
                }
                advertiser.core->_core->AcceptConnection(
                    CppStringFromObjCString(endpointId),
                    PayloadListener{
                        .payload_cb = absl::bind_front(&GNCPayloadListener::OnPayload,
                                                       payload_listener_.get()),
                        .payload_progress_cb = absl::bind_front(
                            &GNCPayloadListener::OnPayloadProgress, payload_listener_.get()),
                    },
                    ResultListener{});
              } else {
                // The connection was rejected by the client.
                advertiser.core->_core->RejectConnection(CppStringFromObjCString(endpointId),
                                                         ResultListener{});
              }
            });
      });
      advertiser.endpoints[endpointId] = endpointInfo;
    }
  }

  void OnAccepted(const std::string &endpoint_id) {
    GNCAdvertiser *advertiser = advertiser_;  // strongify
    if (!advertiser) return;

    NSString *endpointId = ObjCStringFromCppString(endpoint_id);
    GNCAdvertiserEndpointInfo *endpointInfo = advertiser.endpoints[endpointId];
    if (!endpointInfo) {
      GTMLoggerInfo(@"Connection result for unknown endpoint: %@", endpointId);
      return;
    }

    // The connection has been accepted by both endpoints, so create the GNCConnection object
    // and pass it to |successHandler| for the client to use. It will be removed from |endpoints|
    // when the client disconnects (on dealloc of GNCConnection).
    // Note: Use a local strong reference to the connection object; don't just assign to
    // |endpointInfo.connection|. Without a strong reference, the connection object can be
    // deallocated before |successHandler| is called in the Release build.
    __weak __typeof__(advertiser) weakAdvertiser = advertiser;
    id<GNCConnection> connection = [GNCCoreConnection
        connectionWithEndpointId:endpointId
                            core:advertiser.core
                  deallocHandler:^{
                    __strong __typeof__(advertiser) strongAdvertiser = weakAdvertiser;
                    if (!strongAdvertiser) return;
                    [strongAdvertiser.endpoints removeObjectForKey:endpointId];
                  }];
    endpointInfo.connection = connection;

    // Callback is synchronous because it returns the connection handlers.
    dispatch_sync(dispatch_get_main_queue(), ^{
      endpointInfo.connectionHandlers =
          endpointInfo.connectionResultHandlers.successHandler(connection);
    });
  }

  void OnRejected(const std::string &endpoint_id, Status status) {
    GNCAdvertiser *advertiser = advertiser_;  // strongify
    if (!advertiser) return;

    NSString *endpointId = ObjCStringFromCppString(endpoint_id);
    GNCAdvertiserEndpointInfo *endpointInfo = advertiser.endpoints[endpointId];
    if (!endpointInfo) {
      GTMLoggerInfo(@"Connection result for unknown endpoint: %@", endpointId);
      return;
    }

    // One side rejected, so call failureHandler with the connection status (we do this in all
    // cases), and forget the endpoint.
    dispatch_async(dispatch_get_main_queue(), ^{
      endpointInfo.connectionResultHandlers.failureHandler(GNCConnectionFailureRejected);
    });
    [advertiser.endpoints removeObjectForKey:endpointId];
  }

  void OnDisconnected(const std::string &endpoint_id) {
    GNCAdvertiser *advertiser = advertiser_;  // strongify
    if (!advertiser) return;

    NSString *endpointId = ObjCStringFromCppString(endpoint_id);
    GNCAdvertiserEndpointInfo *endpointInfo = advertiser.endpoints[endpointId];
    if (endpointInfo) {
      if (endpointInfo.connection) {
        GNCDisconnectedHandler disconnectedHandler =
            endpointInfo.connectionHandlers.disconnectedHandler;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (disconnectedHandler) disconnectedHandler(GNCDisconnectedReasonUnknown);
        });
      } else {
        GTMLoggerInfo(@"Disconnect for unconnected endpoint: %@", endpointId);
      }
      [advertiser.endpoints removeObjectForKey:endpointId];
    } else {
      GTMLoggerInfo(@"Disconnect for unknown endpoint: %@", endpointId);
    }
  }

  void OnBandwidthChanged(const std::string &endpoint_id, Medium medium) {
    GNCAdvertiser *advertiser = advertiser_;  // strongify
    if (!advertiser) return;

    // TODO(b/169292092): Implement.
  }

 private:
  __weak GNCAdvertiser *advertiser_;
  std::unique_ptr<GNCPayloadListener> payload_listener_;
};

}  // namespace connections
}  // namespace nearby
}  // namespace location

using ::location::nearby::connections::GNCAdvertiserConnectionListener;

@interface GNCAdvertiser () {
  std::unique_ptr<GNCAdvertiserConnectionListener> advertiserListener;
};

@end

@implementation GNCAdvertiser

+ (instancetype)advertiserWithEndpointInfo:(NSData *)endpointInfo
                                 serviceId:(NSString *)serviceId
                                  strategy:(GNCStrategy)strategy
               connectionInitiationHandler:
                   (GNCAdvertiserConnectionInitiationHandler)initiationHandler {
  GNCAdvertiser *advertiser = [[GNCAdvertiser alloc] init];
  advertiser.initiationHandler = initiationHandler;
  advertiser.endpoints = [[NSMutableDictionary alloc] init];
  advertiser.core = GNCGetCore();
  advertiser->advertiserListener = std::make_unique<GNCAdvertiserConnectionListener>(advertiser);

  ConnectionListener listener = {
      .initiated_cb = absl::bind_front(&GNCAdvertiserConnectionListener::OnInitiated,
                                       advertiser->advertiserListener.get()),
      .accepted_cb = absl::bind_front(&GNCAdvertiserConnectionListener::OnAccepted,
                                      advertiser->advertiserListener.get()),
      .rejected_cb = absl::bind_front(&GNCAdvertiserConnectionListener::OnRejected,
                                      advertiser->advertiserListener.get()),
      .disconnected_cb = absl::bind_front(&GNCAdvertiserConnectionListener::OnDisconnected,
                                          advertiser->advertiserListener.get()),
  };

  advertiser.core->_core->StartAdvertising(CppStringFromObjCString(serviceId),
                                           ConnectionOptions{
                                               .strategy = GNCStrategyToStrategy(strategy),
                                               .auto_upgrade_bandwidth = true,
                                               .enforce_topology_constraints = true,
                                           },
                                           ConnectionRequestInfo{
                                               .endpoint_info = ByteArrayFromNSData(endpointInfo),
                                               .listener = std::move(listener),
                                           },
                                           ResultListener{});
  return advertiser;
}

- (void)dealloc {
  GTMLoggerInfo(@"GNCAdvertiser deallocated");
  _core->_core->StopAdvertising(ResultListener{});
}

@end

NS_ASSUME_NONNULL_END