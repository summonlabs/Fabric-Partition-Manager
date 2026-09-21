// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "socket.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

fpm::FrameHeader header_of(std::uint16_t type, std::uint64_t sequence, std::uint64_t epoch,
                           const std::array<std::byte, fpm::session_handle_bytes>& session) {
  fpm::FrameHeader header;
  header.version = fpm::wire_protocol_version;
  header.type = type;
  header.flags = 0;
  header.payload_length = 0;
  header.sequence = sequence;
  header.epoch = epoch;
  header.session = session;
  return header;
}

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.text(text);
  return bytes;
}

}  // namespace

FPM_TEST(protocol, frame_round_trip_is_exact) {
  std::array<std::byte, fpm::session_handle_bytes> session{};
  for (std::size_t index = 0; index < session.size(); ++index) {
    session[index] = static_cast<std::byte>(index * 7 + 1);
  }
  const std::vector<std::byte> payload = bytes_of("payload");
  const fpm::FrameHeader header =
      header_of(static_cast<std::uint16_t>(fpm::MessageType::PublishEvidence), 42, 7, session);
  const std::vector<std::byte> frame = fpm::encode_frame(header, payload, test_limits());
  FPM_CHECK(!frame.empty());
  const fpm::FrameDecodeResult decoded = fpm::decode_frame(frame.data(), frame.size(), test_limits());
  FPM_EQ(decoded.status, fpm::FrameDecodeStatus::Ok);
  FPM_EQ(decoded.header.type, header.type);
  FPM_EQ(decoded.header.sequence, header.sequence);
  FPM_EQ(decoded.header.epoch, header.epoch);
  FPM_CHECK(decoded.header.session == session);
  FPM_EQ(decoded.payload, payload);
  FPM_EQ(decoded.consumed, frame.size());
}

FPM_TEST(protocol, every_truncated_prefix_needs_more_bytes) {
  std::array<std::byte, fpm::session_handle_bytes> session{};
  const fpm::FrameHeader header =
      header_of(static_cast<std::uint16_t>(fpm::MessageType::Assess), 1, 1, session);
  const std::vector<std::byte> frame =
      fpm::encode_frame(header, bytes_of("abcdefghij"), test_limits());
  FPM_CHECK(frame.size() > fpm::frame_overhead_bytes);
  for (std::size_t length = 0; length < frame.size(); ++length) {
    const fpm::FrameDecodeResult decoded = fpm::decode_frame(frame.data(), length, test_limits());
    FPM_EQ(decoded.status, fpm::FrameDecodeStatus::NeedMore);
  }
}

FPM_TEST(protocol, corrupt_headers_are_rejected_with_specific_statuses) {
  std::array<std::byte, fpm::session_handle_bytes> session{};
  const fpm::FrameHeader header =
      header_of(static_cast<std::uint16_t>(fpm::MessageType::Assess), 1, 1, session);
  const std::vector<std::byte> frame = fpm::encode_frame(header, bytes_of("x"), test_limits());

  auto damaged = [&frame](std::size_t index, unsigned mask) {
    std::vector<std::byte> copy = frame;
    copy[index] = static_cast<std::byte>(static_cast<unsigned>(copy[index]) ^ mask);
    return copy;
  };

  const std::vector<std::byte> magic = damaged(0, 0xFF);
  FPM_EQ(fpm::decode_frame(magic.data(), magic.size(), test_limits()).status,
         fpm::FrameDecodeStatus::InvalidMagic);

  const std::vector<std::byte> version = damaged(5, 0x01);
  FPM_EQ(fpm::decode_frame(version.data(), version.size(), test_limits()).status,
         fpm::FrameDecodeStatus::UnsupportedVersion);

  std::vector<std::byte> type = frame;
  type[6] = std::byte{0x7F};
  type[7] = std::byte{0xFF};
  FPM_EQ(fpm::decode_frame(type.data(), type.size(), test_limits()).status,
         fpm::FrameDecodeStatus::TypeOutOfDomain);

  const std::vector<std::byte> flags = damaged(8, 0x02);
  FPM_EQ(fpm::decode_frame(flags.data(), flags.size(), test_limits()).status,
         fpm::FrameDecodeStatus::ReservedNotZero);

  const std::vector<std::byte> reserved = damaged(10, 0x01);
  FPM_EQ(fpm::decode_frame(reserved.data(), reserved.size(), test_limits()).status,
         fpm::FrameDecodeStatus::ReservedNotZero);

  const std::vector<std::byte> integrity = damaged(frame.size() - 1, 0xFF);
  FPM_EQ(fpm::decode_frame(integrity.data(), integrity.size(), test_limits()).status,
         fpm::FrameDecodeStatus::CorruptIntegrity);

  const std::vector<std::byte> body = damaged(fpm::frame_header_bytes, 0xFF);
  FPM_EQ(fpm::decode_frame(body.data(), body.size(), test_limits()).status,
         fpm::FrameDecodeStatus::CorruptIntegrity);
}

FPM_TEST(protocol, an_oversized_declared_payload_is_refused_before_allocation) {
  fpm::Limits limits = test_limits();
  limits.max_frame_bytes = 256;
  std::array<std::byte, fpm::session_handle_bytes> session{};
  fpm::FrameHeader header =
      header_of(static_cast<std::uint16_t>(fpm::MessageType::Assess), 1, 1, session);
  header.payload_length = 4096;
  std::vector<std::byte> frame(fpm::frame_header_bytes + fpm::frame_trailer_bytes, std::byte{0});
  std::memcpy(frame.data(), fpm::frame_magic.data(), fpm::frame_magic.size());
  frame[4] = std::byte{0};
  frame[5] = static_cast<std::byte>(fpm::wire_protocol_version);
  frame[6] = std::byte{0};
  frame[7] = static_cast<std::byte>(static_cast<std::uint16_t>(fpm::MessageType::Assess));
  frame[12] = std::byte{0x00};
  frame[13] = std::byte{0x00};
  frame[14] = std::byte{0x10};
  frame[15] = std::byte{0x00};
  FPM_EQ(fpm::decode_frame(frame.data(), frame.size(), limits).status,
         fpm::FrameDecodeStatus::OversizedPayload);

  // Encoding refuses the same payload rather than producing an unusable frame.
  const std::vector<std::byte> large(1024, std::byte{1});
  FPM_CHECK(fpm::encode_frame(header, large, limits).empty());
}

FPM_TEST(protocol, a_frame_followed_by_more_bytes_reports_the_exact_consumed_length) {
  std::array<std::byte, fpm::session_handle_bytes> session{};
  const fpm::FrameHeader header =
      header_of(static_cast<std::uint16_t>(fpm::MessageType::Assess), 1, 1, session);
  const std::vector<std::byte> first = fpm::encode_frame(header, bytes_of("first"), test_limits());
  const std::vector<std::byte> second = fpm::encode_frame(header, bytes_of("second"), test_limits());
  std::vector<std::byte> stream = first;
  stream.insert(stream.end(), second.begin(), second.end());
  const fpm::FrameDecodeResult decoded =
      fpm::decode_frame(stream.data(), stream.size(), test_limits());
  FPM_EQ(decoded.status, fpm::FrameDecodeStatus::Ok);
  FPM_EQ(decoded.consumed, first.size());
  const fpm::FrameDecodeResult next =
      fpm::decode_frame(stream.data() + decoded.consumed, stream.size() - decoded.consumed,
                        test_limits());
  FPM_EQ(next.status, fpm::FrameDecodeStatus::Ok);
  FPM_EQ(next.payload.size(), std::string("second").size() + 4);
  FPM_EQ(next.consumed, second.size());
}

FPM_TEST(protocol, message_codecs_round_trip) {
  fpm::HelloRequest hello;
  hello.publisher = fpm::PublisherId::from_validated("publisher");
  hello.boot = fpm::PublisherBootId::generate();
  hello.expected_epoch = fpm::CoordinatorEpoch::from_value(9);
  hello.provenance = fpm::Provenance::from_validated("test");
  const auto hello_back =
      fpm::decode_hello_request(fpm::encode_hello_request(hello, test_limits()), test_limits());
  FPM_CHECK(hello_back.has_value());
  FPM_EQ(hello_back->publisher, hello.publisher);
  FPM_EQ(hello_back->expected_epoch, hello.expected_epoch);

  fpm::HelloResponse response;
  response.session = {};
  response.epoch = fpm::CoordinatorEpoch::from_value(3);
  response.boot = fpm::CoordinatorBootId::generate();
  response.protocol_version = fpm::wire_protocol_version;
  response.accepted = true;
  response.reasons.push_back(
      fpm::make_reason(fpm::ReasonCode::EvidenceAccepted, "s", "d", test_limits()));
  const auto response_back = fpm::decode_hello_response(
      fpm::encode_hello_response(response, test_limits()), test_limits());
  FPM_CHECK(response_back.has_value());
  FPM_EQ(response_back->accepted, true);
  FPM_EQ(response_back->reasons.size(), std::size_t{1});

  fpm::OperationResult result;
  result.verdict = fpm::DecisionVerdict::Conflict;
  result.decision = fpm::DecisionId::from_validated("d7");
  result.reasons.push_back(
      fpm::make_reason(fpm::ReasonCode::AuthoritySplitBrainConflict, "p", "two quorums", test_limits()));
  const auto result_back = fpm::decode_operation_result(
      fpm::encode_operation_result(result, test_limits()), test_limits());
  FPM_CHECK(result_back.has_value());
  FPM_EQ(result_back->verdict, fpm::DecisionVerdict::Conflict);
  FPM_EQ(result_back->decision, result.decision);

  const fpm::SyntheticFabric built = fabric({2, 2});
  const auto evidence_back =
      fpm::decode_evidence(fpm::encode_evidence(built.evidence, test_limits()), test_limits());
  FPM_CHECK(evidence_back.has_value());
  FPM_EQ(evidence_back->observations.size(), built.evidence.observations.size());
  FPM_EQ(evidence_back->completeness, built.evidence.completeness);

  fpm::StatusView status;
  status.epoch = fpm::CoordinatorEpoch::from_value(4);
  status.boot = fpm::CoordinatorBootId::generate();
  status.incarnation = fpm::ProcessIncarnationId::generate();
  status.tick = 12;
  status.partition_count = 3;
  status.durable = true;
  const auto status_back =
      fpm::decode_status_view(fpm::encode_status_view(status), test_limits());
  FPM_CHECK(status_back.has_value());
  FPM_EQ(status_back->tick, std::uint64_t{12});
  FPM_EQ(status_back->durable, true);
}

FPM_TEST(protocol, message_decoders_reject_malformed_payloads) {
  const std::vector<std::byte> empty;
  FPM_CHECK(!fpm::decode_hello_request(empty, test_limits()).has_value());
  FPM_CHECK(!fpm::decode_hello_response(empty, test_limits()).has_value());
  FPM_CHECK(!fpm::decode_operation_result(empty, test_limits()).has_value());
  FPM_CHECK(!fpm::decode_evidence(empty, test_limits()).has_value());
  FPM_CHECK(!fpm::decode_status_view(empty, test_limits()).has_value());

  // A verdict outside its domain is refused.
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.u8(200);
  writer.text_id(fpm::DecisionId::from_validated("d1"));
  writer.u32(0);
  FPM_CHECK(!fpm::decode_operation_result(bytes, test_limits()).has_value());

  // Trailing bytes after a complete payload are refused.
  std::vector<std::byte> trailing;
  fpm::CanonicalWriter trailing_writer(trailing);
  trailing_writer.u8(static_cast<std::uint8_t>(fpm::DecisionVerdict::Granted));
  trailing_writer.text_id(fpm::DecisionId::from_validated("d1"));
  trailing_writer.u32(0);
  trailing_writer.u8(0x7F);
  FPM_CHECK(!fpm::decode_operation_result(trailing, test_limits()).has_value());

  // An invalid capability bit outside the defined set is refused.
  fpm::PartitionView view;
  view.id = fpm::PartitionId::from_validated("p0000000000000000000000000000000a");
  view.lineage = fpm::LineageId::from_validated("l0000000000000000000000000000000a");
  view.authorized_bits = 0xFFFF'FFFFu;
  fpm::AssessmentView assessment;
  assessment.partitions.push_back(view);
  const std::vector<std::byte> encoded =
      fpm::encode_assessment_view(assessment, test_limits());
  const auto decoded = fpm::decode_assessment_view(encoded, test_limits());
  FPM_CHECK(!decoded.has_value());
}

FPM_TEST(protocol, a_real_session_binds_identity_and_rejects_a_foreign_handle) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  fpm::ServerOptions server_options;
  server_options.port = 0;
  fpm::PartitionServer server(runtime, server_options);
  std::string error;
  FPM_CHECK_MSG(server.start(error), error);
  FPM_CHECK(server.port() != 0);

  fpm::ClientOptions client_options;
  client_options.port = server.port();
  fpm::PartitionClient client(client_options);
  FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("cli"),
                               fpm::PublisherBootId::generate(),
                               fpm::CoordinatorEpoch{}, error),
                error);
  FPM_CHECK(client.connected());
  FPM_EQ(client.epoch(), runtime.epoch());
  FPM_CHECK(!client.session().is_nil());

  const auto topology = client.adopt_topology(built.topology, error);
  FPM_CHECK_MSG(topology.has_value(), error);
  FPM_EQ(topology->verdict, fpm::DecisionVerdict::Granted);
  const auto evidence = client.publish_evidence(built.evidence, error);
  FPM_CHECK_MSG(evidence.has_value(), error);
  FPM_EQ(evidence->verdict, fpm::DecisionVerdict::Granted);
  const auto assessment = client.assess(error);
  FPM_CHECK_MSG(assessment.has_value(), error);
  FPM_EQ(assessment->verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(assessment->partitions.size(), std::size_t{1});

  const auto status = client.status(error);
  FPM_CHECK_MSG(status.has_value(), error);
  FPM_EQ(status->epoch, runtime.epoch());
  client.close();

  // A raw connection that presents a handle it does not own is refused.
  fpm::detail::SocketRuntime socket_runtime;
  FPM_CHECK(socket_runtime.ok());
  fpm::detail::Socket raw;
  FPM_CHECK_MSG(raw.connect_to("127.0.0.1", server.port(), error), error);
  fpm::HelloRequest hello;
  hello.publisher = fpm::PublisherId::from_validated("intruder");
  hello.boot = fpm::PublisherBootId::generate();
  hello.provenance = fpm::Provenance::from_validated("test");
  const auto send = [&raw](const fpm::FrameHeader& header, const std::vector<std::byte>& body) {
    const std::vector<std::byte> frame = fpm::encode_frame(header, body, test_limits());
    std::string send_error;
    return raw.send_all(frame.data(), frame.size(), send_error);
  };
  std::array<std::byte, fpm::session_handle_bytes> handle{};
  FPM_CHECK(send(fpm::FrameHeader{fpm::wire_protocol_version,
                                  static_cast<std::uint16_t>(fpm::MessageType::Hello), 0, 0, 0, 1, 0,
                                  handle},
                 fpm::encode_hello_request(hello, test_limits())));
  std::array<std::byte, 4096> buffer{};
  std::string read_error;
  const int read = raw.recv_some(buffer.data(), buffer.size(), read_error);
  FPM_CHECK(read > 0);
  const fpm::FrameDecodeResult handshake =
      fpm::decode_frame(buffer.data(), static_cast<std::size_t>(read), test_limits());
  FPM_EQ(handshake.status, fpm::FrameDecodeStatus::Ok);
  const auto accepted =
      fpm::decode_hello_response(handshake.payload, test_limits());
  FPM_CHECK(accepted.has_value());
  FPM_CHECK(accepted->accepted);

  // Present a different handle with a valid frame: the server must refuse.
  std::array<std::byte, fpm::session_handle_bytes> foreign = accepted->session;
  foreign[0] = static_cast<std::byte>(static_cast<unsigned>(foreign[0]) ^ 0xFFu);
  FPM_CHECK(send(fpm::FrameHeader{fpm::wire_protocol_version,
                                  static_cast<std::uint16_t>(fpm::MessageType::Assess), 0, 0, 0, 2,
                                  accepted->epoch.value(), foreign},
                 {}));
  const int rejected_read = raw.recv_some(buffer.data(), buffer.size(), read_error);
  FPM_CHECK(rejected_read > 0);
  const fpm::FrameDecodeResult rejection =
      fpm::decode_frame(buffer.data(), static_cast<std::size_t>(rejected_read), test_limits());
  FPM_EQ(rejection.status, fpm::FrameDecodeStatus::Ok);
  FPM_EQ(rejection.header.type,
         static_cast<std::uint16_t>(fpm::MessageType::ProtocolError));
  raw.shutdown_both();
  raw.close();
  server.stop();
  FPM_CHECK(server.stats().binding_rejections >= 1u);
}

FPM_TEST(protocol, a_replayed_frame_sequence_is_refused) {
  const fpm::SyntheticFabric built = fabric({2});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 2));
  fpm::PartitionRuntime runtime(options);
  fpm::ServerOptions server_options;
  server_options.port = 0;
  fpm::PartitionServer server(runtime, server_options);
  std::string error;
  FPM_CHECK_MSG(server.start(error), error);
  fpm::detail::SocketRuntime socket_runtime;
  fpm::detail::Socket raw;
  FPM_CHECK_MSG(raw.connect_to("127.0.0.1", server.port(), error), error);
  fpm::HelloRequest hello;
  hello.publisher = fpm::PublisherId::from_validated("replayer");
  hello.boot = fpm::PublisherBootId::generate();
  hello.provenance = fpm::Provenance::from_validated("test");
  const auto send = [&raw](const fpm::FrameHeader& header, const std::vector<std::byte>& body) {
    const std::vector<std::byte> frame = fpm::encode_frame(header, body, test_limits());
    std::string send_error;
    return raw.send_all(frame.data(), frame.size(), send_error);
  };
  std::array<std::byte, fpm::session_handle_bytes> handle{};
  FPM_CHECK(send(fpm::FrameHeader{fpm::wire_protocol_version,
                                  static_cast<std::uint16_t>(fpm::MessageType::Hello), 0, 0, 0, 1, 0,
                                  handle},
                 fpm::encode_hello_request(hello, test_limits())));
  std::array<std::byte, 4096> buffer{};
  std::string read_error;
  const int read = raw.recv_some(buffer.data(), buffer.size(), read_error);
  FPM_CHECK(read > 0);
  const fpm::FrameDecodeResult handshake =
      fpm::decode_frame(buffer.data(), static_cast<std::size_t>(read), test_limits());
  const auto accepted = fpm::decode_hello_response(handshake.payload, test_limits());
  FPM_CHECK(accepted.has_value());

  const fpm::FrameHeader first{fpm::wire_protocol_version,
                               static_cast<std::uint16_t>(fpm::MessageType::QueryStatus), 0, 0, 0, 5,
                               accepted->epoch.value(), accepted->session};
  FPM_CHECK(send(first, {}));
  const int ok_read = raw.recv_some(buffer.data(), buffer.size(), read_error);
  FPM_CHECK(ok_read > 0);
  // Replay the identical sequence number.
  FPM_CHECK(send(first, {}));
  const int replay_read = raw.recv_some(buffer.data(), buffer.size(), read_error);
  FPM_CHECK(replay_read > 0);
  const fpm::FrameDecodeResult rejection =
      fpm::decode_frame(buffer.data(), static_cast<std::size_t>(replay_read), test_limits());
  FPM_EQ(rejection.header.type, static_cast<std::uint16_t>(fpm::MessageType::ProtocolError));
  raw.shutdown_both();
  raw.close();
  server.stop();
  FPM_CHECK(server.stats().replay_rejections >= 1u);
}
