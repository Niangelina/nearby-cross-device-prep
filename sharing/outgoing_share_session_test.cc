// Copyright 2024 Google LLC
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

#include "sharing/outgoing_share_session.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "protobuf-matchers/protocol-buffer-matchers.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "internal/analytics/mock_event_logger.h"
#include "internal/analytics/sharing_log_matchers.h"
#include "internal/test/fake_clock.h"
#include "internal/test/fake_task_runner.h"
#include "sharing/analytics/analytics_recorder.h"
#include "sharing/attachment_container.h"
#include "sharing/fake_nearby_connection.h"
#include "sharing/fake_nearby_connections_manager.h"
#include "sharing/file_attachment.h"
#include "sharing/nearby_connections_manager.h"
#include "sharing/nearby_connections_types.h"
#include "sharing/nearby_file_handler.h"
#include "sharing/nearby_sharing_decoder_impl.h"
#include "sharing/paired_key_verification_runner.h"
#include "sharing/proto/wire_format.pb.h"
#include "sharing/share_target.h"
#include "sharing/text_attachment.h"
#include "sharing/transfer_metadata.h"
#include "sharing/transfer_metadata_matchers.h"
#include "sharing/wifi_credentials_attachment.h"

namespace nearby::sharing {
namespace {
using ::location::nearby::proto::sharing::EventCategory;
using ::location::nearby::proto::sharing::EventType;
using ::location::nearby::proto::sharing::OSType;
using ::nearby::analytics::HasCategory;
using ::nearby::analytics::HasEventType;
using ::nearby::analytics::HasSessionId;
using ::nearby::sharing::analytics::proto::SharingLog;
using ::nearby::sharing::service::proto::ConnectionResponseFrame;
using ::nearby::sharing::service::proto::Frame;
using ::nearby::sharing::service::proto::IntroductionFrame;
using ::nearby::sharing::service::proto::ProgressUpdateFrame;
using ::nearby::sharing::service::proto::V1Frame;
using ::nearby::sharing::service::proto::WifiCredentials;
using ::testing::_;
using ::testing::AllOf;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::IsEmpty;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Matcher;
using ::testing::MockFunction;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::StrictMock;

constexpr absl::string_view kEndpointId = "ABCD";

class OutgoingShareSessionTest : public ::testing::Test {
 public:
  OutgoingShareSessionTest()
      : session_(fake_task_runner_, analytics_recorder_,
                 std::string(kEndpointId), share_target_,
                 transfer_metadata_callback_.AsStdFunction()),
        text1_(nearby::sharing::service::proto::TextMetadata::URL,
               "A bit of text body", "Some text title", "text/html"),
        text2_(nearby::sharing::service::proto::TextMetadata::ADDRESS,
               "A bit of text body 2", "Some text title 2", "text/plain"),
        file1_("/usr/local/tmp/someFileName.jpg", "/usr/local/parent"),
        file2_("/usr/local/tmp/someFileName2.jpg", "/usr/local/parent2"),
        wifi1_(
            "GoogleGuest",
            nearby::sharing::service::proto::WifiCredentialsMetadata::WPA_PSK,
            "somepassword", /*is_hidden=*/true) {
    AttachmentContainer container(
        std::vector<TextAttachment>{text1_, text2_},
        std::vector<FileAttachment>{file1_},
        std::vector<WifiCredentialsAttachment>{wifi1_});
    session_.SetAttachmentContainer(std::move(container));
  }

 protected:
  FakeClock fake_clock_;
  FakeTaskRunner fake_task_runner_ {&fake_clock_, 1};
  nearby::analytics::MockEventLogger mock_event_logger_;
  analytics::AnalyticsRecorder analytics_recorder_{/*vendor_id=*/0,
                                                   &mock_event_logger_};
  NearbySharingDecoderImpl decoder_;
  ShareTarget share_target_;
  MockFunction<void(OutgoingShareSession&, const TransferMetadata&)>
      transfer_metadata_callback_;
  OutgoingShareSession session_;
  TextAttachment text1_;
  TextAttachment text2_;
  FileAttachment file1_;
  FileAttachment file2_;
  WifiCredentialsAttachment wifi1_;
};

TEST_F(OutgoingShareSessionTest, GetFilePaths) {
  OutgoingShareSession session(
      fake_task_runner_, analytics_recorder_, std::string(kEndpointId),
      share_target_, [](OutgoingShareSession&, const TransferMetadata&) {});
  AttachmentContainer container(std::vector<TextAttachment>{},
                                std::vector<FileAttachment>{file1_, file2_},
                                std::vector<WifiCredentialsAttachment>{});
  session.SetAttachmentContainer(std::move(container));

  auto file_paths = session.GetFilePaths();

  ASSERT_THAT(file_paths, SizeIs(2));
  EXPECT_THAT(file_paths[0], Eq(file1_.file_path()));
  EXPECT_THAT(file_paths[1], Eq(file2_.file_path()));
}

TEST_F(OutgoingShareSessionTest, CreateTextPayloadsWithNoTextAttachments) {
  OutgoingShareSession session(
      fake_task_runner_, analytics_recorder_, std::string(kEndpointId),
      share_target_, [](OutgoingShareSession&, const TransferMetadata&) {});
  session.CreateTextPayloads();
  const std::vector<Payload>& payloads = session.text_payloads();

  EXPECT_THAT(payloads, IsEmpty());
}

TEST_F(OutgoingShareSessionTest, CreateTextPayloads) {
  session_.CreateTextPayloads();
  const std::vector<Payload>& payloads = session_.text_payloads();
  auto& attachment_payload_map = session_.attachment_payload_map();

  ASSERT_THAT(payloads, SizeIs(2));
  EXPECT_THAT(payloads[0].content.type, Eq(PayloadContent::Type::kBytes));
  EXPECT_THAT(payloads[1].content.type, Eq(PayloadContent::Type::kBytes));
  EXPECT_THAT(payloads[0].content.bytes_payload.bytes,
              Eq(std::vector<uint8_t>(text1_.text_body().begin(),
                                      text1_.text_body().end())));
  EXPECT_THAT(payloads[1].content.bytes_payload.bytes,
              Eq(std::vector<uint8_t>(text2_.text_body().begin(),
                                      text2_.text_body().end())));

  ASSERT_THAT(attachment_payload_map, SizeIs(2));
  ASSERT_THAT(attachment_payload_map.contains(text1_.id()), IsTrue());
  EXPECT_THAT(attachment_payload_map.at(text1_.id()), Eq(payloads[0].id));
  ASSERT_THAT(attachment_payload_map.contains(text2_.id()), IsTrue());
  EXPECT_THAT(attachment_payload_map.at(text2_.id()), Eq(payloads[1].id));
}

TEST_F(OutgoingShareSessionTest, CreateFilePayloadsWithNoFileAttachments) {
  OutgoingShareSession session(
      fake_task_runner_, analytics_recorder_, std::string(kEndpointId),
      share_target_, [](OutgoingShareSession&, const TransferMetadata&) {});

  EXPECT_THAT(
      session.CreateFilePayloads(std::vector<NearbyFileHandler::FileInfo>()),
      IsTrue());
  const std::vector<Payload>& payloads = session.file_payloads();

  EXPECT_THAT(payloads, IsEmpty());
}

TEST_F(OutgoingShareSessionTest, CreateFilePayloadsWithWrongFileInfo) {
  EXPECT_THAT(
      session_.CreateFilePayloads(std::vector<NearbyFileHandler::FileInfo>()),
      IsFalse());
  const std::vector<Payload>& payloads = session_.file_payloads();

  EXPECT_THAT(payloads, IsEmpty());
}

TEST_F(OutgoingShareSessionTest, CreateFilePayloads) {
  std::vector<NearbyFileHandler::FileInfo> file_infos;
  file_infos.push_back({
      .size = 12355L,
      .file_path = file1_.file_path().value(),
  });
  session_.CreateFilePayloads(file_infos);
  const std::vector<Payload>& payloads = session_.file_payloads();
  auto& attachment_payload_map = session_.attachment_payload_map();

  ASSERT_THAT(payloads, SizeIs(1));
  EXPECT_THAT(payloads[0].content.type, Eq(PayloadContent::Type::kFile));
  EXPECT_THAT(payloads[0].content.file_payload.size, Eq(12355L));
  EXPECT_THAT(payloads[0].content.file_payload.parent_folder,
              Eq(file1_.parent_folder()));
  EXPECT_THAT(payloads[0].content.file_payload.file.path,
              Eq(file1_.file_path()));

  EXPECT_THAT(attachment_payload_map, SizeIs(1));
  ASSERT_THAT(attachment_payload_map.contains(file1_.id()), IsTrue());
  EXPECT_THAT(attachment_payload_map.at(file1_.id()), Eq(payloads[0].id));

  EXPECT_THAT(session_.attachment_container().GetFileAttachments()[0].size(),
              Eq(12355L));
}

TEST_F(OutgoingShareSessionTest, CreateWifiPayloadsWithNoWifiAttachments) {
  OutgoingShareSession session(
      fake_task_runner_, analytics_recorder_, std::string(kEndpointId),
      share_target_, [](OutgoingShareSession&, const TransferMetadata&) {});
  session.CreateWifiCredentialsPayloads();
  const std::vector<Payload>& payloads = session.file_payloads();

  EXPECT_THAT(payloads, IsEmpty());
}

TEST_F(OutgoingShareSessionTest, CreateWifiCredentialsPayloads) {
  session_.CreateWifiCredentialsPayloads();
  const std::vector<Payload>& payloads = session_.wifi_credentials_payloads();
  auto& attachment_payload_map = session_.attachment_payload_map();

  ASSERT_THAT(payloads, SizeIs(1));
  EXPECT_THAT(payloads[0].content.type, Eq(PayloadContent::Type::kBytes));
  WifiCredentials wifi_credentials;
  EXPECT_THAT(wifi_credentials.ParseFromArray(
                  payloads[0].content.bytes_payload.bytes.data(),
                  payloads[0].content.bytes_payload.bytes.size()),
              IsTrue());
  EXPECT_THAT(wifi_credentials.password(), Eq(wifi1_.password()));
  EXPECT_THAT(wifi_credentials.has_hidden_ssid(), Eq(wifi1_.is_hidden()));

  ASSERT_THAT(attachment_payload_map, SizeIs(1));
  ASSERT_THAT(attachment_payload_map.contains(wifi1_.id()), IsTrue());
  EXPECT_THAT(attachment_payload_map.at(wifi1_.id()), Eq(payloads[0].id));
}

TEST_F(OutgoingShareSessionTest, SendIntroductionWithoutPayloads) {
  EXPECT_THAT(session_.SendIntroduction([]() {}), IsFalse());
}

TEST_F(OutgoingShareSessionTest, SendIntroductionSuccess) {
  session_.set_session_id(1234);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  std::vector<NearbyFileHandler::FileInfo> file_infos;
  file_infos.push_back({
      .size = 12355L,
      .file_path = file1_.file_path().value(),
  });
  session_.CreateFilePayloads(file_infos);
  session_.CreateTextPayloads();
  session_.CreateWifiCredentialsPayloads();
  EXPECT_CALL(
      mock_event_logger_,
      Log(Matcher<const SharingLog&>(AllOf(
          (HasCategory(EventCategory::SENDING_EVENT),
           HasEventType(EventType::SEND_INTRODUCTION),
           Property(&SharingLog::send_introduction, HasSessionId(1234)))))));

  EXPECT_THAT(session_.SendIntroduction([]() {}), IsTrue());

  std::vector<uint8_t> frame_data = connection.GetWrittenData();
  Frame frame;
  ASSERT_THAT(frame.ParseFromArray(frame_data.data(), frame_data.size()),
              IsTrue());
  ASSERT_THAT(frame.version(), Eq(Frame::V1));
  ASSERT_THAT(frame.v1().type(), Eq(V1Frame::INTRODUCTION));
  const IntroductionFrame& intro_frame = frame.v1().introduction();
  EXPECT_THAT(intro_frame.start_transfer(), IsTrue());
  const std::vector<Payload>& text_payloads = session_.text_payloads();
  ASSERT_THAT(intro_frame.text_metadata_size(), Eq(2));
  EXPECT_THAT(intro_frame.text_metadata(0).id(), Eq(text1_.id()));
  EXPECT_THAT(intro_frame.text_metadata(0).text_title(),
              Eq(text1_.text_title()));
  EXPECT_THAT(intro_frame.text_metadata(0).type(), Eq(text1_.type()));
  EXPECT_THAT(intro_frame.text_metadata(0).size(), Eq(text1_.size()));
  EXPECT_THAT(intro_frame.text_metadata(0).payload_id(),
              Eq(text_payloads[0].id));

  EXPECT_THAT(intro_frame.text_metadata(1).id(), Eq(text2_.id()));
  EXPECT_THAT(intro_frame.text_metadata(1).text_title(),
              Eq(text2_.text_title()));
  EXPECT_THAT(intro_frame.text_metadata(1).type(), Eq(text2_.type()));
  EXPECT_THAT(intro_frame.text_metadata(1).size(), Eq(text2_.size()));
  EXPECT_THAT(intro_frame.text_metadata(1).payload_id(),
              Eq(text_payloads[1].id));

  const std::vector<Payload>& file_payloads = session_.file_payloads();
  ASSERT_THAT(intro_frame.file_metadata_size(), Eq(1));
  EXPECT_THAT(intro_frame.file_metadata(0).id(), Eq(file1_.id()));
  // File attachment size has been updated by CreateFilePayloads().
  EXPECT_THAT(intro_frame.file_metadata(0).size(), Eq(file_infos[0].size));
  EXPECT_THAT(intro_frame.file_metadata(0).name(), Eq(file1_.file_name()));
  EXPECT_THAT(intro_frame.file_metadata(0).payload_id(),
              Eq(file_payloads[0].id));
  EXPECT_THAT(intro_frame.file_metadata(0).type(), Eq(file1_.type()));
  EXPECT_THAT(intro_frame.file_metadata(0).mime_type(), Eq(file1_.mime_type()));

  const std::vector<Payload>& wifi_payloads =
      session_.wifi_credentials_payloads();
  ASSERT_THAT(intro_frame.wifi_credentials_metadata_size(), Eq(1));
  EXPECT_THAT(intro_frame.wifi_credentials_metadata(0).id(), Eq(wifi1_.id()));
  EXPECT_THAT(intro_frame.wifi_credentials_metadata(0).ssid(),
              Eq(wifi1_.ssid()));
  EXPECT_THAT(intro_frame.wifi_credentials_metadata(0).security_type(),
              Eq(wifi1_.security_type()));
  EXPECT_THAT(intro_frame.wifi_credentials_metadata(0).payload_id(),
              Eq(wifi_payloads[0].id));
}

TEST_F(OutgoingShareSessionTest, SendIntroductionTimeout) {
  AttachmentContainer container(
      std::vector<TextAttachment>{text1_}, {}, {});
  session_.SetAttachmentContainer(std::move(container));
  session_.set_session_id(1234);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  session_.CreateTextPayloads();
  EXPECT_CALL(
      mock_event_logger_,
      Log(Matcher<const SharingLog&>(AllOf(
          (HasCategory(EventCategory::SENDING_EVENT),
           HasEventType(EventType::SEND_INTRODUCTION),
           Property(&SharingLog::send_introduction, HasSessionId(1234)))))));

  bool accept_timeout_called = false;
  EXPECT_THAT(session_.SendIntroduction(
                  [&accept_timeout_called]() { accept_timeout_called = true; }),
              IsTrue());

  fake_clock_.FastForward(absl::Seconds(60));
  fake_task_runner_.SyncWithTimeout(absl::Milliseconds(100));

  EXPECT_THAT(accept_timeout_called, IsTrue());
}

TEST_F(OutgoingShareSessionTest, SendIntroductionTimeoutCancelled) {
  AttachmentContainer container(
      std::vector<TextAttachment>{text1_}, {}, {});
  session_.SetAttachmentContainer(std::move(container));
  session_.set_session_id(1234);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  session_.CreateTextPayloads();
  EXPECT_CALL(
      mock_event_logger_,
      Log(Matcher<const SharingLog&>(AllOf(
          (HasCategory(EventCategory::SENDING_EVENT),
           HasEventType(EventType::SEND_INTRODUCTION),
           Property(&SharingLog::send_introduction, HasSessionId(1234)))))));

  bool accept_timeout_called = false;
  EXPECT_THAT(session_.SendIntroduction(
                  [&accept_timeout_called]() { accept_timeout_called = true; }),
              IsTrue());
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::ACCEPT);
  EXPECT_CALL(transfer_metadata_callback_,
              Call(_, HasStatus(TransferMetadata::Status::kInProgress)));

  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);
  EXPECT_THAT(status.has_value(), IsFalse());

  fake_clock_.FastForward(absl::Seconds(60));
  fake_task_runner_.SyncWithTimeout(absl::Milliseconds(100));

  EXPECT_THAT(accept_timeout_called, IsFalse());
}

TEST_F(OutgoingShareSessionTest, AcceptTransferNotConnected) {
  EXPECT_THAT(
      session_.AcceptTransfer([](std::optional<ConnectionResponseFrame>) {}),
      IsFalse());
}

TEST_F(OutgoingShareSessionTest, AcceptTransferNotReady) {
  session_.set_session_id(1234);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);

  EXPECT_THAT(
      session_.AcceptTransfer([](std::optional<ConnectionResponseFrame>) {}),
      IsFalse());
}

TEST_F(OutgoingShareSessionTest, AcceptTransferSuccess) {
  AttachmentContainer container(
      std::vector<TextAttachment>{text1_}, {}, {});
  session_.SetAttachmentContainer(std::move(container));
  session_.set_session_id(1234);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  session_.CreateTextPayloads();
  EXPECT_CALL(mock_event_logger_,
              Log(Matcher<const SharingLog&>(
                  AllOf((HasCategory(EventCategory::SENDING_EVENT),
                         HasEventType(EventType::SEND_INTRODUCTION))))));
  EXPECT_THAT(session_.SendIntroduction([]() {}), IsTrue());
  EXPECT_CALL(
      transfer_metadata_callback_,
      Call(_, HasStatus(TransferMetadata::Status::kAwaitingRemoteAcceptance)));

  bool connection_response_received = false;
  EXPECT_THAT(
      session_.AcceptTransfer([&connection_response_received](
                                  std::optional<ConnectionResponseFrame>) {
        connection_response_received = true;
      }),
      IsTrue());

  // Send response frame
  nearby::sharing::service::proto::Frame frame =
      nearby::sharing::service::proto::Frame();
  frame.set_version(nearby::sharing::service::proto::Frame::V1);
  V1Frame* v1frame = frame.mutable_v1();
  v1frame->set_type(service::proto::V1Frame::RESPONSE);
  v1frame->mutable_connection_response();
  std::vector<uint8_t> data;
  data.resize(frame.ByteSizeLong());
  EXPECT_THAT(frame.SerializeToArray(data.data(), data.size()), IsTrue());
  connection.AppendReadableData(std::move(data));

  EXPECT_THAT(connection_response_received, IsTrue());
}

TEST_F(OutgoingShareSessionTest, HandleConnectionResponseEmptyResponse) {
  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(std::nullopt);

  ASSERT_THAT(status.has_value(), IsTrue());
  EXPECT_THAT(
      status.value(),
      Eq(TransferMetadata::Status::kFailedToReadOutgoingConnectionResponse));
}

TEST_F(OutgoingShareSessionTest, HandleConnectionResponseRejectResponse) {
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::REJECT);
  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);

  ASSERT_THAT(status.has_value(), IsTrue());
  EXPECT_THAT(status.value(), Eq(TransferMetadata::Status::kRejected));
}

TEST_F(OutgoingShareSessionTest,
       HandleConnectionResponseNotEnoughSpaceResponse) {
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::NOT_ENOUGH_SPACE);
  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);

  ASSERT_THAT(status.has_value(), IsTrue());
  EXPECT_THAT(status.value(), Eq(TransferMetadata::Status::kNotEnoughSpace));
}

TEST_F(OutgoingShareSessionTest,
       HandleConnectionResponseUnsuportedTypeResponse) {
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::UNSUPPORTED_ATTACHMENT_TYPE);
  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);

  ASSERT_THAT(status.has_value(), IsTrue());
  EXPECT_THAT(status.value(),
              Eq(TransferMetadata::Status::kUnsupportedAttachmentType));
}

TEST_F(OutgoingShareSessionTest, HandleConnectionResponseTimeoutResponse) {
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::TIMED_OUT);
  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);

  ASSERT_THAT(status.has_value(), IsTrue());
  EXPECT_THAT(status.value(), Eq(TransferMetadata::Status::kTimedOut));
}

TEST_F(OutgoingShareSessionTest, HandleConnectionResponseAcceptResponse) {
  ConnectionResponseFrame response;
  response.set_status(ConnectionResponseFrame::ACCEPT);
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  EXPECT_CALL(transfer_metadata_callback_,
              Call(_, HasStatus(TransferMetadata::Status::kInProgress)));

  std::optional<TransferMetadata::Status> status =
      session_.HandleConnectionResponse(response);

  ASSERT_THAT(status.has_value(), IsFalse());

  // Verify progress update frame
  std::vector<uint8_t> frame_data = connection.GetWrittenData();
  Frame frame;
  ASSERT_THAT(frame.ParseFromArray(frame_data.data(), frame_data.size()),
              IsTrue());
  ASSERT_THAT(frame.version(), Eq(Frame::V1));
  ASSERT_THAT(frame.v1().type(), Eq(V1Frame::PROGRESS_UPDATE));
  const ProgressUpdateFrame& progress_frame = frame.v1().progress_update();
  EXPECT_THAT(progress_frame.start_transfer(), IsTrue());
}

TEST_F(OutgoingShareSessionTest, SendPayloadsDisableCancellationOptimization) {
  session_.set_session_id(1234);
  std::vector<NearbyFileHandler::FileInfo> file_infos;
  file_infos.push_back({
      .size = 12355L,
      .file_path = file1_.file_path().value(),
  });
  session_.CreateFilePayloads(file_infos);
  session_.CreateTextPayloads();
  session_.CreateWifiCredentialsPayloads();
  MockFunction<void(int64_t, TransferMetadata)> transfer_metadata_callback;
  StrictMock<MockFunction<void(
      std::unique_ptr<Payload>,
      std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>)>>
      send_payload_callback;
  FakeNearbyConnectionsManager connections_manager;
  connections_manager.set_send_payload_callback(
      send_payload_callback.AsStdFunction());
  EXPECT_CALL(send_payload_callback, Call(_, _))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(file1_.id());
          }))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(text1_.id());
          }))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(text2_.id());
          }));
  EXPECT_CALL(mock_event_logger_,
              Log(Matcher<const SharingLog&>(
                  AllOf((HasCategory(EventCategory::SENDING_EVENT),
                         HasEventType(EventType::SEND_ATTACHMENTS_START),
                         Property(&SharingLog::send_attachments_start,
                                  HasSessionId(1234)))))));
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);

  session_.SendPayloads(
      /*enable_transfer_cancellation_optimization=*/
      false, &fake_clock_, connections_manager,
      [](std::optional<V1Frame> frame) {},
      transfer_metadata_callback.AsStdFunction());

  auto payload_listener = session_.payload_tracker().lock();
  EXPECT_THAT(payload_listener, IsTrue());
}

TEST_F(OutgoingShareSessionTest, SendPayloadsEnableCancellationOptimization) {
  session_.set_session_id(1234);
  std::vector<NearbyFileHandler::FileInfo> file_infos;
  file_infos.push_back({
      .size = 12355L,
      .file_path = file1_.file_path().value(),
  });
  session_.CreateFilePayloads(file_infos);
  session_.CreateTextPayloads();
  session_.CreateWifiCredentialsPayloads();
  MockFunction<void(int64_t, TransferMetadata)> transfer_metadata_callback;
  StrictMock<MockFunction<void(
      std::unique_ptr<Payload>,
      std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>)>>
      send_payload_callback;
  FakeNearbyConnectionsManager connections_manager;
  connections_manager.set_send_payload_callback(
      send_payload_callback.AsStdFunction());
  EXPECT_CALL(send_payload_callback, Call(_, _))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(file1_.id());
          }));
  EXPECT_CALL(mock_event_logger_,
              Log(Matcher<const SharingLog&>(
                  AllOf((HasCategory(EventCategory::SENDING_EVENT),
                         HasEventType(EventType::SEND_ATTACHMENTS_START),
                         Property(&SharingLog::send_attachments_start,
                                  HasSessionId(1234)))))));
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);

  session_.SendPayloads(
      /*enable_transfer_cancellation_optimization=*/
      true, &fake_clock_, connections_manager,
      [](std::optional<V1Frame> frame) {},
      transfer_metadata_callback.AsStdFunction());

  auto payload_listener = session_.payload_tracker().lock();
  EXPECT_THAT(payload_listener, IsTrue());
}

TEST_F(OutgoingShareSessionTest, SendNextPayload) {
  session_.set_session_id(1234);
  std::vector<NearbyFileHandler::FileInfo> file_infos;
  file_infos.push_back({
      .size = 12355L,
      .file_path = file1_.file_path().value(),
  });
  session_.CreateFilePayloads(file_infos);
  session_.CreateTextPayloads();
  session_.CreateWifiCredentialsPayloads();
  MockFunction<void(int64_t, TransferMetadata)> transfer_metadata_callback;
  StrictMock<MockFunction<void(
      std::unique_ptr<Payload>,
      std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>)>>
      send_payload_callback;
  FakeNearbyConnectionsManager connections_manager;
  connections_manager.set_send_payload_callback(
      send_payload_callback.AsStdFunction());

  EXPECT_CALL(send_payload_callback, Call(_, _))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(file1_.id());
          }));
  EXPECT_CALL(mock_event_logger_,
              Log(Matcher<const SharingLog&>(
                  AllOf((HasCategory(EventCategory::SENDING_EVENT),
                         HasEventType(EventType::SEND_ATTACHMENTS_START),
                         Property(&SharingLog::send_attachments_start,
                                  HasSessionId(1234)))))));
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);

  session_.SendPayloads(
      /*enable_transfer_cancellation_optimization=*/
      true, &fake_clock_, connections_manager,
      [](std::optional<V1Frame> frame) {},
      transfer_metadata_callback.AsStdFunction());

  EXPECT_CALL(send_payload_callback, Call(_, _))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(text1_.id());
          }));
  session_.SendNextPayload(connections_manager);

  EXPECT_CALL(send_payload_callback, Call(_, _))
      .WillOnce(Invoke(
          [this](
              std::unique_ptr<Payload> payload,
              std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>) {
            payload->id = session_.attachment_payload_map().at(text2_.id());
          }));
  session_.SendNextPayload(connections_manager);
}

TEST_F(OutgoingShareSessionTest, ProcessKeyVerificationResultFail) {
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  session_.SetTokenForTests("1234");

  EXPECT_THAT(
      session_.ProcessKeyVerificationResult(
          PairedKeyVerificationRunner::PairedKeyVerificationResult::kFail,
          OSType::WINDOWS),
      IsFalse());

  EXPECT_THAT(session_.token(), Eq("1234"));
  EXPECT_THAT(session_.os_type(), Eq(OSType::WINDOWS));
}

TEST_F(OutgoingShareSessionTest, ProcessKeyVerificationResultSuccess) {
  FakeNearbyConnection connection;
  session_.OnConnected(decoder_, absl::Now(), &connection);
  session_.SetTokenForTests("1234");

  EXPECT_THAT(
      session_.ProcessKeyVerificationResult(
          PairedKeyVerificationRunner::PairedKeyVerificationResult::kSuccess,
          OSType::WINDOWS),
      IsTrue());

  EXPECT_THAT(session_.token(), Eq("1234"));
  EXPECT_THAT(session_.os_type(), Eq(OSType::WINDOWS));
}

}  // namespace
}  // namespace nearby::sharing
