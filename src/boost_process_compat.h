/**
 * @file src/boost_process_compat.h
 * @brief Includes Boost.Process while isolating third-party deprecation diagnostics.
 */
#pragma once

#if defined(__APPLE__) && defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wdeprecated-declarations"

  // Boost.Process v1 still uses std::codecvt_utf8 on macOS, including in Boost 1.92.
  #include <boost/process/v1/locale.hpp>

  #pragma clang diagnostic pop
#endif

#if defined(__HAIKU__)
  // Boost.Process v1 marks a child that has not exited yet with the wait status 0x017f, which
  // reads as "stopped" with the Linux/BSD bit layout. Haiku packs wait statuses differently
  // (exit code in bits 0-7, terminating signal in bits 8-15, stop signal in bits 16-23), so
  // there 0x017f reads as "killed by signal 1" and every freshly started child reports
  // running() == false. Sunshine would then treat launched apps as exited immediately.
  //
  // This replaces boost/process/v1/detail/posix/is_running.hpp (Boost 1.86 and later, same
  // include guard) with a copy whose marker has Haiku's "stopped" layout. Everything else is
  // unchanged. Distributed under the Boost Software License, Version 1.0.
  #ifndef BOOST_PROCESS_DETAIL_POSIX_IS_RUNNING_HPP
    #define BOOST_PROCESS_DETAIL_POSIX_IS_RUNNING_HPP

    #include <boost/process/v1/detail/config.hpp>
    #include <boost/process/v1/detail/posix/child_handle.hpp>
    #include <sys/wait.h>
    #include <system_error>

namespace boost {
  namespace process {
    BOOST_PROCESS_V1_INLINE namespace v1 {
      namespace detail {
        namespace posix {

          // Stop signal 1 in Haiku's layout; any real waitpid() result without WUNTRACED differs.
          constexpr int still_active = 0x00010000;
          static_assert(WIFSTOPPED(still_active), "Expected still_active to indicate WIFSTOPPED");
          static_assert(!WIFEXITED(still_active), "Expected still_active to not indicate WIFEXITED");
          static_assert(!WIFSIGNALED(still_active), "Expected still_active to not indicate WIFSIGNALED");
          static_assert(!WIFCONTINUED(still_active), "Expected still_active to not indicate WIFCONTINUED");

          inline bool is_running(int code) {
            return !WIFEXITED(code) && !WIFSIGNALED(code);
          }

          inline bool is_running(const child_handle &p, int &exit_code, std::error_code &ec) noexcept {
            int status;
            auto ret = ::waitpid(p.pid, &status, WNOHANG);

            if (ret == -1) {
              if (errno != ECHILD) {  // because it no child is running, then this one isn't either, obviously.
                ec = ::boost::process::v1::detail::get_last_error();
              }
              return false;
            } else if (ret == 0) {
              return true;
            } else {
              ec.clear();

              if (!is_running(status)) {
                exit_code = status;
              }

              return false;
            }
          }

          inline bool is_running(const child_handle &p, int &exit_code) {
            std::error_code ec;
            bool b = is_running(p, exit_code, ec);
            boost::process::v1::detail::throw_error(ec, "waitpid(2) failed in is_running");
            return b;
          }

          inline int eval_exit_status(int code) {
            if (WIFEXITED(code)) {
              return WEXITSTATUS(code);
            } else if (WIFSIGNALED(code)) {
              return WTERMSIG(code);
            } else {
              return code;
            }
          }

        }  // namespace posix
      }  // namespace detail
    }  // namespace v1
  }  // namespace process
}  // namespace boost

  #endif
#endif

#include <boost/process/v1.hpp>
