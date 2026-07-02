#include "daemon_state.hpp"

#include <cassert>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>

void run_daemon_event_tests() {
  using namespace wmux::daemon_internal;

  DaemonState state;
  DaemonEventLoop events{state};
  events.start();

  const bool mutation_allowed_inside_event = events.call([](DaemonState&) {
    return daemon_state_mutation_allowed();
  });
  assert(mutation_allowed_inside_event);

#ifndef NDEBUG
  bool mutation_asserted = false;
  try {
    assert_daemon_state_mutation_allowed("daemon_event_test.outside_event_loop");
  } catch (const std::logic_error&) {
    mutation_asserted = true;
  }
  assert(mutation_asserted);
#endif

  constexpr wmux::ClientId kClientId = 11;
  constexpr wmux::SessionId kSessionId = 22;
  events.call([](DaemonState& daemon_state) {
    DaemonState::AttachClientRuntime client;
    client.client.id = 11;
    client.client.attached_session = 22;
    daemon_state.attach_clients.emplace(11, std::move(client));
  });

  std::vector<std::uint8_t> input{'d', 'i', 'r', '\r'};
  const auto input_event = DaemonEvent::client_input(kClientId, kSessionId, std::move(input));
  assert(input_event.client_id == kClientId);
  assert(input_event.session_id == kSessionId);
  assert(input_event.bytes.size() == 4);

  (void)events.call_event(DaemonEvent::set_paste_buffer("alpha\r\nbeta"));
  const auto paste_buffer = events.call([](DaemonState& daemon_state) {
    return daemon_state.paste_buffer;
  });
  assert(paste_buffer.text == "alpha\r\nbeta");
  assert(paste_buffer.id == 1);
  assert(paste_buffer.source == wmux::PasteBufferSource::CopyMode);

  const auto resize_result = events.call_event(DaemonEvent::client_resize(kClientId, 123, 45));
  assert(!resize_result.has_response);
  assert(!resize_result.request_shutdown);
  assert(resize_result.handled);
  assert(resize_result.changed);

  const auto size = events.call([=](DaemonState& daemon_state) {
    const auto client = daemon_state.attach_clients.find(kClientId);
    assert(client != daemon_state.attach_clients.end());
    return std::pair<std::uint16_t, std::uint16_t>{
        client->second.client.size.columns,
        client->second.client.size.rows};
  });
  assert(size.first == 123);
  assert(size.second == 45);

  const auto duplicate_resize =
      events.call_event(DaemonEvent::client_resize(kClientId, 123, 45));
  assert(!duplicate_resize.has_response);
  assert(!duplicate_resize.request_shutdown);
  assert(duplicate_resize.handled);
  assert(!duplicate_resize.changed);

  DaemonKeyEvent key_event;
  key_event.name = "prefix-c";
  key_event.bytes = {'c'};
  (void)events.call_event(DaemonEvent::decoded_key(kClientId, std::move(key_event)));
  const auto key_events = events.call([](DaemonState& daemon_state) {
    std::lock_guard lock(daemon_state.mutex);
    return diagnostic_events_snapshot(
        daemon_state.diagnostics,
        DiagnosticEventCategory::Key);
  });
  assert(!key_events.empty());
  assert(key_events.back().event_type == "decoded_key");
  assert(key_events.back().client_id == kClientId);
  assert(key_events.back().session_id == kSessionId);

  const auto resize_metrics = events.call([](DaemonState& daemon_state) {
    return std::pair<std::uint64_t, std::uint64_t>{
        daemon_state.render_metrics.client_resize_events.load(std::memory_order_relaxed),
        daemon_state.render_metrics.client_resize_noops.load(std::memory_order_relaxed)};
  });
  assert(resize_metrics.first == 2);
  assert(resize_metrics.second == 1);

  const auto stale_resize = events.call_event(DaemonEvent::client_resize(999, 1, 1));
  assert(!stale_resize.has_response);
  assert(!stale_resize.request_shutdown);

  events.call([](DaemonState& daemon_state) {
    for (std::size_t index = 0; index < kDiagnosticEventRingCapacity + 5; ++index) {
      record_diagnostic_event(
          daemon_state,
          DiagnosticEvent{
              0,
              {},
              DiagnosticEventCategory::Command,
              "info",
              "test_command",
              static_cast<wmux::RequestId>(index),
              0,
              0,
              0,
              0,
              "test",
              {}});
    }
  });
  const auto command_events = events.call([](DaemonState& daemon_state) {
    std::lock_guard lock(daemon_state.mutex);
    return diagnostic_events_snapshot(
        daemon_state.diagnostics,
        DiagnosticEventCategory::Command);
  });
  assert(command_events.size() == kDiagnosticEventRingCapacity);
  assert(command_events.front().request_id == 5);

  const auto shutdown = events.call_event(DaemonEvent::shutdown());
  assert(!shutdown.has_response);
  assert(shutdown.request_shutdown);

  const auto disconnect =
      events.call_event(DaemonEvent::client_disconnected(kClientId, AttachEndReason::Detached));
  assert(!disconnect.has_response);
  assert(!disconnect.request_shutdown);

  const auto client_count = events.call([](DaemonState& daemon_state) {
    return daemon_state.attach_clients.size();
  });
  assert(client_count == 0);

  events.stop();
}
