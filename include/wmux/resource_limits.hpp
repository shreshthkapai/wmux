#pragma once

#include <cstddef>
#include <cstdint>

namespace wmux {

constexpr std::size_t kMaxPaneRawOutputBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxPaneScrollbackLines = 2000;
constexpr std::size_t kMaxPasteBufferBytes = 1024 * 1024;
constexpr std::size_t kMaxConfigLineBytes = 16 * 1024;
constexpr std::uint16_t kMaxAttachTerminalColumns = 512;
constexpr std::uint16_t kMaxAttachTerminalRows = 256;

constexpr std::uint32_t kMaxAttachInputPayloadBytes = 64 * 1024;
constexpr std::uint32_t kMaxAttachCommandPayloadBytes = 4 * 1024;
constexpr std::uint32_t kMaxAttachResizePayloadBytes = 4;
constexpr std::uint32_t kMaxAttachMousePayloadBytes = 8;
constexpr std::uint32_t kMaxAttachScrollPayloadBytes = 1;
constexpr std::uint32_t kMaxAttachCopyModePayloadBytes = 1;
constexpr std::uint32_t kMaxAttachPastePayloadBytes = 0;
constexpr std::uint32_t kMaxAttachFramePayloadSize = 1024 * 1024;
constexpr std::size_t kMaxAttachRenderFrameBytes = 4 * 1024 * 1024;

}  // namespace wmux
