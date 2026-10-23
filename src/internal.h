// license:BSD-3-Clause
// copyright-holders:CHDmodern contributors
// Helpers shared by the C API's source files (not installed).
#pragma once

#include <string>
#include <system_error>

namespace chdm_internal {

// Records message (or the code's description) as the calling thread's last error; returns code.
int set_error(int code, std::string message = std::string()) noexcept;
// A MAME / system error condition as a chdm_error, recording its message.
int translate(std::error_condition const &err) noexcept;
// Call from a catch block: the current exception as a chdm_error.
int current_exception_error() noexcept;

} // namespace chdm_internal
