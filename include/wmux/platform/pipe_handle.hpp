#pragma once

namespace wmux {

// Opaque transport handle used by daemon state/events. Windows maps this to a
// HANDLE in the platform implementation; core daemon state must not include
// windows.h just to store a client pipe identity.
using PlatformPipeHandle = void*;

inline constexpr PlatformPipeHandle kNullPlatformPipeHandle = nullptr;

}  // namespace wmux
