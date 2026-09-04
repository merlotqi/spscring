#pragma once

#include <cstdint>

namespace spscing {

enum class reserve_status {
  ok = 0,
  full,
  timeout,
  message_too_large
};

struct reserve_result {
  reserve_status status{reserve_status::full};
  void* payload{nullptr};
  std::uint64_t position{0};

  explicit operator bool() const noexcept { return status == reserve_status::ok; }
};

inline const char* reserve_status_cstr(reserve_status status) noexcept {
  switch (status) {
    case reserve_status::ok:
      return "ok";
    case reserve_status::full:
      return "full";
    case reserve_status::timeout:
      return "timeout";
    case reserve_status::message_too_large:
      return "message_too_large";
    default:
      return "unknown";
  }
}

}  // namespace spscing
