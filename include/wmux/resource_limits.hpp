#pragma once

#include <cstddef>
#include <cstdint>

namespace wmux {

constexpr std::size_t kMaxPaneRawOutputBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxPaneScrollbackLines = 2000;
constexpr std::size_t kMaxPasteBufferBytes = 1024 * 1024;
constexpr std::size_t kMaxPasteWriteChunkBytes = 16 * 1024;
constexpr std::uint32_t kPasteWriteChunkDelayMs = 1;
constexpr std::size_t kMaxConfigLineBytes = 16 * 1024;
constexpr std::uint16_t kMaxAttachTerminalColumns = 512;
constexpr std::uint16_t kMaxAttachTerminalRows = 256;

constexpr std::uint32_t kMaxIpcFramePayloadBytes = 4 * 1024 * 1024;
constexpr std::uint32_t kMaxControlIpcPayloadBytes = 4 * 1024 * 1024;
constexpr std::uint32_t kMaxIpcErrorPayloadBytes = 64 * 1024;

constexpr std::uint32_t kMaxAttachInputPayloadBytes = 64 * 1024;
constexpr std::uint32_t kMaxAttachCommandPayloadBytes = 4 * 1024;
constexpr std::uint32_t kMaxAttachResizePayloadBytes = 4;
constexpr std::uint32_t kMaxAttachMousePayloadBytes = 8;
constexpr std::uint32_t kMaxAttachScrollPayloadBytes = 1;
constexpr std::uint32_t kMaxAttachCopyModePayloadBytes = 1;
constexpr std::uint32_t kMaxAttachPastePayloadBytes = 0;
constexpr std::uint32_t kMaxAttachFramePayloadSize = 1024 * 1024;
constexpr std::size_t kMaxAttachRenderFrameBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxAttachPendingOutputBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxAttachPendingOutputFrames = 8;

constexpr std::size_t kMaxSessions = 64;
constexpr std::size_t kMaxWindowsPerSession = 64;
constexpr std::size_t kMaxPanesPerWindow = 64;
constexpr std::size_t kMaxLogFileBytes = 32 * 1024 * 1024;

struct ResourceLimits {
  std::size_t max_sessions{kMaxSessions};
  std::size_t max_windows_per_session{kMaxWindowsPerSession};
  std::size_t max_panes_per_window{kMaxPanesPerWindow};
  std::size_t max_pane_raw_output_bytes{kMaxPaneRawOutputBytes};
  std::size_t max_pane_scrollback_lines{kMaxPaneScrollbackLines};
  std::size_t max_paste_buffer_bytes{kMaxPasteBufferBytes};
  std::size_t max_ipc_frame_payload_bytes{kMaxIpcFramePayloadBytes};
  std::size_t max_client_output_queue_bytes{kMaxAttachPendingOutputBytes};
  std::size_t max_client_output_queue_frames{kMaxAttachPendingOutputFrames};
  std::size_t max_attach_render_frame_bytes{kMaxAttachRenderFrameBytes};
  std::size_t max_log_file_bytes{kMaxLogFileBytes};
};

}  // namespace wmux
