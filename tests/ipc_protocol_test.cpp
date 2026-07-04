#include "wmux/ipc_protocol.hpp"

#include <cassert>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expects_ping_request_json() {
  const auto request = wmux::parse_request_json(wmux::make_ping_request_json());
  assert(request);
  assert(request->type == "Ping");
}

void expects_command_request_json() {
  const std::vector<std::string_view> args{"rename-session", "-t", "finance", "trading"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "RenameSession");
  assert(request->target_name == "finance");
  assert(request->new_name == "trading");
}

void expects_forced_command_request_json() {
  const std::vector<std::string_view> args{"server", "stop", "--force"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "ServerStop");
  assert(request->force);
}

void expects_window_command_request_json() {
  const std::vector<std::string_view> args{"new-window", "-t", "finance", "-n", "logs"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "NewWindow");
  assert(request->session_name == "finance");
  assert(request->window_name == "logs");
}

void expects_window_selection_request_json() {
  const std::vector<std::string_view> args{"select-window", "-t", "finance:1"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "SelectWindow");
  assert(request->target_name == "finance:1");
}

void expects_split_command_request_json() {
  const std::vector<std::string_view> args{"split-window", "-t", "finance", "-v"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "SplitWindow");
  assert(request->session_name == "finance");
  assert(request->split_direction == "vertical");
}

void expects_resize_command_request_json() {
  const std::vector<std::string_view> args{"resize-pane", "-t", "finance:1", "-R"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "ResizePane");
  assert(request->target_name == "finance:1");
  assert(request->resize_direction == "-R");
}

void expects_set_option_request_json() {
  const std::vector<std::string_view> args{"set", "-g", "mouse", "on"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "SetOption");
  assert(request->option_name == "mouse");
  assert(request->option_value == "on");
}

void expects_bind_key_request_json() {
  const std::vector<std::string_view> args{"bind-key", "E", "select-layout", "-E"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "BindKey");
  assert(request->key_name == "E");
  assert(request->key_action == "select-layout -E");
}

void expects_unbind_key_request_json() {
  const std::vector<std::string_view> args{"unbind-key", "e"};
  const auto command = wmux::parse_command_line(args);
  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->type == "UnbindKey");
  assert(request->key_name == "e");
  assert(request->key_action.empty());
}

void expects_dump_command_request_json() {
  {
    const std::vector<std::string_view> args{"dump-state"};
    const auto command = wmux::parse_command_line(args);
    const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
    assert(request);
    assert(request->type == "DumpState");
  }
  {
    const std::vector<std::string_view> args{"dump-layout"};
    const auto command = wmux::parse_command_line(args);
    const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
    assert(request);
    assert(request->type == "DumpLayout");
  }
  {
    const std::vector<std::string_view> args{"dump-events"};
    const auto command = wmux::parse_command_line(args);
    const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
    assert(request);
    assert(request->type == "DumpEvents");
  }
}

void expects_attach_request_json_with_terminal_size() {
  wmux::CommandLine command;
  command.kind = wmux::CommandKind::AttachSession;
  command.session_name = "finance";
  wmux::TerminalCapabilities capabilities;
  capabilities.host = wmux::TerminalHost::Alacritty;
  capabilities.supports_truecolor = true;
  capabilities.supports_sgr_mouse = true;

  const auto request =
      wmux::parse_request_json(wmux::make_attach_request_json(command, 132, 43, capabilities));
  assert(request);
  assert(request->type == "AttachStart");
  assert(request->session_name == "finance");
  assert(request->terminal_columns == 132);
  assert(request->terminal_rows == 43);
  assert(request->terminal_capabilities_provided);
  assert(request->terminal_capabilities.host == wmux::TerminalHost::Alacritty);
  assert(request->terminal_capabilities.supports_truecolor);
  assert(request->terminal_capabilities.supports_sgr_mouse);
}

void expects_json_escaping() {
  wmux::CommandLine command;
  command.kind = wmux::CommandKind::NewSession;
  command.session_name = "quote\"slash\\line\n";

  const auto request = wmux::parse_request_json(wmux::make_command_request_json(command));
  assert(request);
  assert(request->session_name == "quote\"slash\\line\n");
}

void ignores_nested_false_positive_json_fields() {
  const auto request = wmux::parse_request_json(
      "{\"meta\":{\"type\":\"KillSession\"},\"type\":\"ListSessions\"}\n");

  assert(request);
  assert(request->type == "ListSessions");
}

void rejects_json_fields_with_wrong_type() {
  const auto request = wmux::parse_request_json(
      "{\"type\":{\"nested\":\"ListSessions\"},\"session_name\":\"finance\"}\n");

  assert(!request);
}

void expects_response_json() {
  const auto response = wmux::parse_response_json(wmux::make_response_json(true, "wmux: ok\n"));
  assert(response);
  assert(response->ok);
  assert(response->message == "wmux: ok\n");
  assert(!response->mouse_enabled);
  assert(response->prefix == "C-b");
  assert(response->status_bar_enabled);
  assert(response->escape_time_ms == 50);
}

void expects_attach_response_json_with_settings() {
  const auto enabled = wmux::parse_response_json(wmux::make_response_json(true, "", true));
  assert(enabled);
  assert(enabled->ok);
  assert(enabled->message.empty());
  assert(enabled->mouse_enabled);
  assert(enabled->prefix == "C-b");
  assert(enabled->status_bar_enabled);

  const auto disabled = wmux::parse_response_json(wmux::make_response_json(true, "", false));
  assert(disabled);
  assert(disabled->ok);
  assert(disabled->message.empty());
  assert(!disabled->mouse_enabled);

  const auto configured =
      wmux::parse_response_json(
          wmux::make_response_json(true, "", true, "C-a", false, 75, "7a\tnew-window\n"));
  assert(configured);
  assert(configured->ok);
  assert(configured->mouse_enabled);
  assert(configured->prefix == "C-a");
  assert(!configured->status_bar_enabled);
  assert(configured->escape_time_ms == 75);
  assert(configured->key_bindings == "7a\tnew-window\n");
}

void expects_ipc_control_frame() {
  const auto payload = wmux::make_ping_request_json();
  const auto frame = wmux::make_ipc_frame(wmux::IpcFrameKind::Control, 42, payload);
  assert(frame.size() == wmux::kIpcFrameHeaderSize + payload.size());

  const auto header =
      wmux::parse_ipc_frame_header(std::string_view{frame.data(), wmux::kIpcFrameHeaderSize});
  assert(header.ok);
  assert(header.header.version == wmux::kIpcProtocolVersion);
  assert(header.header.kind == wmux::IpcFrameKind::Control);
  assert(header.header.request_id == 42);
  assert(header.header.payload_size == payload.size());

  const auto parsed = wmux::parse_ipc_frame(frame);
  assert(parsed.ok);
  assert(parsed.payload == payload);
  assert(parsed.header.request_id == 42);
}

void expects_ipc_error_frame() {
  const auto payload = wmux::make_response_json(false, "wmux: no\n");
  const auto frame = wmux::make_ipc_frame(wmux::IpcFrameKind::Error, 77, payload);
  const auto parsed = wmux::parse_ipc_frame(frame);
  assert(parsed.ok);
  assert(parsed.header.kind == wmux::IpcFrameKind::Error);
  assert(parsed.header.request_id == 77);
  assert(parsed.payload == payload);
}

void names_ipc_frame_kinds_and_errors() {
  assert(wmux::ipc_frame_kind_name(wmux::IpcFrameKind::Control) == "Control");
  assert(wmux::ipc_frame_kind_name(wmux::IpcFrameKind::AttachInput) == "AttachInput");
  assert(wmux::ipc_frame_kind_name(wmux::IpcFrameKind::AttachOutput) == "AttachOutput");
  assert(wmux::ipc_frame_error_name(wmux::IpcFrameError::BadMagic) == "BadMagic");
  assert(wmux::ipc_frame_error_name(wmux::IpcFrameError::OversizedPayload) ==
         "OversizedPayload");
}

std::string make_bad_ipc_header(
    std::string_view magic,
    std::uint16_t version,
    std::uint8_t kind,
    std::uint64_t request_id,
    std::uint32_t payload_size) {
  std::string header;
  header.reserve(wmux::kIpcFrameHeaderSize);
  header.append(magic);
  header.resize(4, '\0');
  header.push_back(static_cast<char>(version & 0xFF));
  header.push_back(static_cast<char>((version >> 8) & 0xFF));
  header.push_back(static_cast<char>(kind));
  for (int shift = 0; shift < 64; shift += 8) {
    header.push_back(static_cast<char>((request_id >> shift) & 0xFF));
  }
  header.push_back(static_cast<char>(payload_size & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 8) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 16) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 24) & 0xFF));
  return header;
}

void rejects_bad_ipc_frames() {
  assert(wmux::parse_ipc_frame_header("WM").error == wmux::IpcFrameError::PartialHeader);

  const auto bad_magic = make_bad_ipc_header(
      "NOPE",
      wmux::kIpcProtocolVersion,
      static_cast<std::uint8_t>(wmux::IpcFrameKind::Control),
      1,
      0);
  assert(wmux::parse_ipc_frame_header(bad_magic).error == wmux::IpcFrameError::BadMagic);

  const auto bad_version = make_bad_ipc_header(
      "WMUX",
      wmux::kIpcProtocolVersion + 1,
      static_cast<std::uint8_t>(wmux::IpcFrameKind::Control),
      2,
      0);
  const auto bad_version_result = wmux::parse_ipc_frame_header(bad_version);
  assert(bad_version_result.error == wmux::IpcFrameError::UnsupportedVersion);
  assert(bad_version_result.header.request_id == 2);

  const auto bad_kind =
      make_bad_ipc_header("WMUX", wmux::kIpcProtocolVersion, 99, 3, 0);
  const auto bad_kind_result = wmux::parse_ipc_frame_header(bad_kind);
  assert(bad_kind_result.error == wmux::IpcFrameError::UnknownKind);
  assert(bad_kind_result.header.request_id == 3);

  const auto huge = make_bad_ipc_header(
      "WMUX",
      wmux::kIpcProtocolVersion,
      static_cast<std::uint8_t>(wmux::IpcFrameKind::Control),
      4,
      wmux::kMaxControlIpcPayloadBytes + 1);
  assert(wmux::parse_ipc_frame_header(huge).error == wmux::IpcFrameError::OversizedPayload);

  const auto truncated =
      wmux::make_ipc_frame(wmux::IpcFrameKind::Control, 5, "abc").substr(0, wmux::kIpcFrameHeaderSize + 1);
  assert(wmux::parse_ipc_frame(truncated).error == wmux::IpcFrameError::TruncatedPayload);
}

void fuzzes_ipc_frame_parser_with_random_bytes() {
  std::mt19937 rng{1337};
  std::uniform_int_distribution<int> byte_distribution{0, 255};
  std::uniform_int_distribution<int> size_distribution{0, 256};

  for (int iteration = 0; iteration < 512; ++iteration) {
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(size_distribution(rng)));
    for (char& byte : bytes) {
      byte = static_cast<char>(byte_distribution(rng));
    }
    (void)wmux::parse_ipc_frame(bytes);
    if (bytes.size() == wmux::kIpcFrameHeaderSize) {
      (void)wmux::parse_ipc_frame_header(bytes);
    }
  }
}

void expects_framed_attach_input_payload() {
  const auto attach_payload = wmux::make_attach_input_frame("dir\r\n");
  const auto frame =
      wmux::make_ipc_frame(wmux::IpcFrameKind::AttachInput, 91, attach_payload);
  const auto parsed = wmux::parse_ipc_frame(frame);
  assert(parsed.ok);
  assert(parsed.header.kind == wmux::IpcFrameKind::AttachInput);
  assert(parsed.header.request_id == 91);
  assert(parsed.payload == attach_payload);

  const auto attach_header = wmux::parse_attach_frame_header(
      std::string_view{parsed.payload.data(), wmux::kAttachFrameHeaderSize});
  assert(attach_header);
  assert(attach_header->type == wmux::AttachFrameType::Input);
  assert(attach_header->payload_size == 5);
  assert(parsed.payload.substr(wmux::kAttachFrameHeaderSize) == "dir\r\n");
}

void expects_framed_attach_output_payload() {
  const std::string payload = "\x1b[2Jwmux render frame";
  const auto frame = wmux::make_ipc_frame(wmux::IpcFrameKind::AttachOutput, 92, payload);
  const auto parsed = wmux::parse_ipc_frame(frame);
  assert(parsed.ok);
  assert(parsed.header.kind == wmux::IpcFrameKind::AttachOutput);
  assert(parsed.header.request_id == 92);
  assert(parsed.payload == payload);
}

void expects_attach_input_frame() {
  const auto frame = wmux::make_attach_input_frame("dir\r\n");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 5);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Input);
  assert(header->payload_size == 5);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "dir\r\n");
}

void expects_attach_detach_frame() {
  const auto frame = wmux::make_attach_detach_frame();
  assert(frame.size() == wmux::kAttachFrameHeaderSize);

  const auto header = wmux::parse_attach_frame_header(frame);
  assert(header);
  assert(header->type == wmux::AttachFrameType::Detach);
  assert(header->payload_size == 0);
}

void expects_attach_command_frame() {
  const auto frame = wmux::make_attach_command_frame("next-window");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 11);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Command);
  assert(header->payload_size == 11);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "next-window");
}

void expects_attach_resize_frame() {
  const auto frame = wmux::make_attach_resize_frame(132, 43);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 4);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Resize);
  assert(header->payload_size == 4);

  const auto dimensions =
      wmux::parse_attach_resize_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(dimensions);
  assert(dimensions->first == 132);
  assert(dimensions->second == 43);
}

void expects_attach_status_frame() {
  const auto frame = wmux::make_attach_status_frame(":rename-session trading");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 23);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Status);
  assert(header->payload_size == 23);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == ":rename-session trading");
}

void expects_attach_command_mode_frame() {
  const auto frame = wmux::make_attach_command_mode_frame("new-window -n logs");
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 18);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::CommandMode);
  assert(header->payload_size == 18);
  assert(frame.substr(wmux::kAttachFrameHeaderSize) == "new-window -n logs");
}

void expects_attach_mouse_focus_frame() {
  const auto frame = wmux::make_attach_mouse_focus_frame(61, 8);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 4);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::MouseFocus);
  assert(header->payload_size == 4);

  const auto focus =
      wmux::parse_attach_mouse_focus_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(focus);
  assert(focus->column == 61);
  assert(focus->row == 8);
}

void expects_attach_mouse_event_frame() {
  const wmux::AttachMouseEventPayload payload{
      61,
      8,
      32,
      wmux::AttachMouseButton::Left,
      wmux::AttachMouseAction::Drag};
  const auto frame = wmux::make_attach_mouse_event_frame(payload);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 8);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::MouseEvent);
  assert(header->payload_size == 8);

  const auto mouse =
      wmux::parse_attach_mouse_event_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(mouse);
  assert(mouse->column == 61);
  assert(mouse->row == 8);
  assert(mouse->button_code == 32);
  assert(mouse->button == wmux::AttachMouseButton::Left);
  assert(mouse->action == wmux::AttachMouseAction::Drag);
}

void expects_attach_scroll_frame() {
  const auto frame = wmux::make_attach_scroll_frame(wmux::AttachScrollAction::PageUp);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 1);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::Scroll);
  assert(header->payload_size == 1);

  const auto scroll =
      wmux::parse_attach_scroll_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(scroll);
  assert(*scroll == wmux::AttachScrollAction::PageUp);
}

void expects_attach_copy_mode_frame() {
  const auto frame =
      wmux::make_attach_copy_mode_frame(wmux::AttachCopyModeAction::CopySelection);
  assert(frame.size() == wmux::kAttachFrameHeaderSize + 1);

  const auto header = wmux::parse_attach_frame_header(
      std::string_view{frame.data(), wmux::kAttachFrameHeaderSize});
  assert(header);
  assert(header->type == wmux::AttachFrameType::CopyMode);
  assert(header->payload_size == 1);

  const auto action =
      wmux::parse_attach_copy_mode_payload(frame.substr(wmux::kAttachFrameHeaderSize));
  assert(action);
  assert(*action == wmux::AttachCopyModeAction::CopySelection);
}

void expects_attach_paste_frame() {
  const auto frame = wmux::make_attach_paste_frame();
  assert(frame.size() == wmux::kAttachFrameHeaderSize);

  const auto header = wmux::parse_attach_frame_header(frame);
  assert(header);
  assert(header->type == wmux::AttachFrameType::Paste);
  assert(header->payload_size == 0);
}

void names_attach_frame_types() {
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Input) == "Input");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Resize) == "Resize");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::MouseEvent) == "Mouse");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Detach) == "Detach");
  assert(wmux::attach_frame_type_name(wmux::AttachFrameType::Paste) == "Paste");
}

void rejects_bad_attach_resize_payload() {
  assert(!wmux::parse_attach_resize_payload(""));
  assert(!wmux::parse_attach_resize_payload(std::string_view{"\0\0\x18\0", 4}));
  assert(!wmux::parse_attach_resize_payload(std::string_view{"\x01\x03\x18\0", 4}));
}

void rejects_oversized_attach_frame_payloads() {
  std::string header;
  header.reserve(wmux::kAttachFrameHeaderSize);
  header.push_back('W');
  header.push_back('M');
  header.push_back(static_cast<char>(wmux::AttachFrameType::Input));
  const auto payload_size = wmux::kMaxAttachInputPayloadBytes + 1;
  header.push_back(static_cast<char>(payload_size & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 8) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 16) & 0xFF));
  header.push_back(static_cast<char>((payload_size >> 24) & 0xFF));

  assert(!wmux::parse_attach_frame_header(header));
}

void rejects_bad_attach_mouse_focus_payload() {
  assert(!wmux::parse_attach_mouse_focus_payload(""));
  assert(!wmux::parse_attach_mouse_focus_payload(std::string_view{"\0\0\x08\0", 4}));
  assert(!wmux::parse_attach_mouse_focus_payload(std::string_view{"\x3d\0\0\0", 4}));
}

void rejects_bad_attach_mouse_event_payload() {
  assert(!wmux::parse_attach_mouse_event_payload(""));
  assert(!wmux::parse_attach_mouse_event_payload(std::string_view{"\x01\0\x01\0\0\0\x07\0", 8}));
  assert(!wmux::parse_attach_mouse_event_payload(std::string_view{"\x01\0\x01\0\0\0\0\x04", 8}));
}

void rejects_bad_attach_scroll_payload() {
  assert(!wmux::parse_attach_scroll_payload(""));
  assert(!wmux::parse_attach_scroll_payload(std::string_view{"\x05", 1}));
}

void rejects_bad_attach_copy_mode_payload() {
  assert(!wmux::parse_attach_copy_mode_payload(""));
  assert(!wmux::parse_attach_copy_mode_payload(std::string_view{"\xff", 1}));
}

void expects_bad_json_rejected() {
  const auto request = wmux::parse_request_json("{\"nope\":\"ListSessions\"}\n");
  assert(!request);
}

}  // namespace

void run_ipc_protocol_tests() {
  expects_ping_request_json();
  expects_command_request_json();
  expects_forced_command_request_json();
  expects_window_command_request_json();
  expects_window_selection_request_json();
  expects_split_command_request_json();
  expects_resize_command_request_json();
  expects_set_option_request_json();
  expects_bind_key_request_json();
  expects_unbind_key_request_json();
  expects_dump_command_request_json();
  expects_attach_request_json_with_terminal_size();
  expects_json_escaping();
  ignores_nested_false_positive_json_fields();
  rejects_json_fields_with_wrong_type();
  expects_response_json();
  expects_attach_response_json_with_settings();
  expects_ipc_control_frame();
  expects_ipc_error_frame();
  names_ipc_frame_kinds_and_errors();
  rejects_bad_ipc_frames();
  fuzzes_ipc_frame_parser_with_random_bytes();
  expects_framed_attach_input_payload();
  expects_framed_attach_output_payload();
  expects_attach_input_frame();
  expects_attach_detach_frame();
  expects_attach_command_frame();
  expects_attach_resize_frame();
  expects_attach_status_frame();
  expects_attach_command_mode_frame();
  expects_attach_mouse_focus_frame();
  expects_attach_mouse_event_frame();
  expects_attach_scroll_frame();
  expects_attach_copy_mode_frame();
  expects_attach_paste_frame();
  names_attach_frame_types();
  rejects_bad_attach_resize_payload();
  rejects_oversized_attach_frame_payloads();
  rejects_bad_attach_mouse_focus_payload();
  rejects_bad_attach_mouse_event_payload();
  rejects_bad_attach_scroll_payload();
  rejects_bad_attach_copy_mode_payload();
  expects_bad_json_rejected();
}
