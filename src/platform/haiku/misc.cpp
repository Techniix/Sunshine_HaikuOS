/**
 * @file src/platform/haiku/misc.cpp
 * @brief Miscellaneous definitions for Haiku.
 */

// standard includes
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <arpa/inet.h>
#include <FindDirectory.h>
#include <ifaddrs.h>
#include <image.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <OS.h>
#include <poll.h>
#include <StorageDefs.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <lizardbyte/common/env.h>

// local includes
#include "app_context.h"
#include "src/boost_process_compat.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/permissions.h"

using namespace std::literals;
namespace fs = std::filesystem;
namespace bp = boost::process::v1;

namespace platf {
  namespace haiku {
    /**
     * @brief Check whether libvirtualhid's input_server add-on is listening.
     * @details Mirrors the port name in libvirtualhid's haiku_input_protocol.hpp.
     */
    bool input_addon_running() {
      return find_port("libvirtualhid:input") >= 0;
    }
  }  // namespace haiku

  std::vector<permission_status_t> get_permission_statuses() {
    // Haiku has no per-application input or screen-capture permissions. Input availability is
    // reported in the log at startup instead (see init()), because the Web UI's generic "input"
    // permission help describes Linux device groups.
    return {};
  }

  bool request_permission(std::string_view id) {
    (void) id;
    return false;
  }

  /**
   * @brief Owning pointer for `getifaddrs` results.
   */
  using ifaddr_t = util::safe_ptr<ifaddrs, &freeifaddrs>;

  /**
   * @brief Read the local interface address list.
   *
   * @return Owning pointer to the interface address list, or nullptr on failure.
   */
  ifaddr_t get_ifaddrs() {
    ifaddrs *p {nullptr};
    if (getifaddrs(&p) != 0) {
      return ifaddr_t {nullptr};
    }
    return ifaddr_t {p};
  }

  /**
   * @brief Returns the Sunshine settings directory.
   * @details This is used for the log directory, so it cannot invoke Boost logging!
   *          Haiku keeps per-user settings in ~/config/settings; XDG_CONFIG_HOME is honored for
   *          people who share a configuration with other systems.
   * @return The path of the appdata directory that should be used.
   */
  fs::path appdata() {
    static std::once_flag once;
    static fs::path config_path;

    std::call_once(once, []() {
      if (std::string dir; lizardbyte::common::get_env("XDG_CONFIG_HOME", dir) && !dir.empty()) {
        config_path = fs::path(dir) / "sunshine"sv;
        return;
      }

      std::array<char, B_PATH_NAME_LENGTH> settings {};
      if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, settings.data(), settings.size()) == B_OK) {
        config_path = fs::path(settings.data()) / "Sunshine"sv;
        return;
      }

      // find_directory() cannot fail on a working system; keep a sane fallback anyway.
      config_path = fs::path("/boot/home/config/settings/Sunshine");
      std::cerr << "find_directory(B_USER_SETTINGS_DIRECTORY) failed, using "sv << config_path << std::endl;
    });

    return config_path;
  }

  std::filesystem::path get_xdg_restore_token_path() {
    // Haiku has no XDG desktop portal; keep the path well-defined for shared code.
    return appdata() / "portal_token";
  }

  std::string from_sockaddr(const sockaddr *const ip_addr) {
    std::array<char, INET6_ADDRSTRLEN> data = {};

    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data.data(), INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data.data(), INET_ADDRSTRLEN);
    }

    return std::string {data.data()};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    std::array<char, INET6_ADDRSTRLEN> data = {};

    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data.data(), INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data.data(), INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, std::string {data.data()}};
  }

  std::string get_mac_address(const std::string_view &address) {
    auto ifaddrs = get_ifaddrs();

    // Haiku's getifaddrs() reports an AF_LINK entry per interface, like the BSDs.
    std::string interface_name;
    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && pos->ifa_addr->sa_family != AF_LINK && address == from_sockaddr(pos->ifa_addr)) {
        interface_name = pos->ifa_name;
        break;
      }
    }

    if (!interface_name.empty()) {
      for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
        if (pos->ifa_addr && pos->ifa_addr->sa_family == AF_LINK && interface_name == pos->ifa_name) {
          auto sdl = (struct sockaddr_dl *) pos->ifa_addr;
          auto mac = (unsigned char *) LLADDR(sdl);
          if (sdl->sdl_alen == 0) {
            break;
          }

          std::ostringstream mac_stream;
          mac_stream << std::hex << std::setfill('0');
          for (int i = 0; i < sdl->sdl_alen; i++) {
            if (i > 0) {
              mac_stream << ':';
            }
            mac_stream << std::setw(2) << (int) mac[i];
          }
          return mac_stream.str();
        }
      }
    }

    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

  /**
   * @brief Launch a command with the caller's privileges.
   * @details Haiku has no privilege separation between desktop programs, so the elevation and
   *          interactivity flags have no meaning here.
   *          Unlike Linux, bp::limit_handles is not used: Boost implements it by listing
   *          /dev/fd, which Haiku does not provide, and the launch would fail.
   */
  bp::child run_command(bool, bool, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    // clang-format off
    if (!group) {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, ec);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, ec);
      }
    }
    else {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, ec, *group);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, ec, *group);
      }
    }
    // clang-format on
  }

  void open_url(const std::string &url) {
    // Haiku's `open` command hands URLs to the preferred application for their MIME type.
    auto working_dir = boost::filesystem::path(lizardbyte::common::get_env("HOME"));
    std::string cmd = R"(open ")" + url + R"(")";

    bp::environment _env = boost::this_process::environment();
    std::error_code ec;
    auto child = run_command(false, false, cmd, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't open url ["sv << url << "]: System: "sv << ec.message();
    } else {
      BOOST_LOG(info) << "Opened url ["sv << url << "]"sv;
      child.detach();
    }
  }

  void adjust_thread_priority(thread_priority_e priority) {
    // Haiku schedules by thread priority rather than nice values. Stay below the real-time range
    // (100+), which could starve app_server and input_server if a capture loop misbehaved.
    using enum thread_priority_e;
    int32 haiku_priority;
    switch (priority) {
      case low:
        haiku_priority = B_LOW_PRIORITY;
        break;
      case normal:
        haiku_priority = B_NORMAL_PRIORITY;
        break;
      case high:
        haiku_priority = B_DISPLAY_PRIORITY;
        break;
      case critical:
        haiku_priority = B_URGENT_DISPLAY_PRIORITY;
        break;
      default:
        BOOST_LOG(debug) << "Unknown thread priority: "sv << std::to_underlying(priority);
        return;
    }

    if (auto status = set_thread_priority(find_thread(nullptr), haiku_priority); status < B_OK) {
      BOOST_LOG(warning) << "set_thread_priority("sv << haiku_priority << ") failed: "sv << strerror(status);
    }
  }

  void set_thread_name(std::string_view name) {
    std::string truncated {name.substr(0, B_OS_NAME_LENGTH - 1)};
    rename_thread(find_thread(nullptr), truncated.c_str());
  }

  void enable_mouse_keys() {
    // Haiku has no mouse-keys accessibility feature to toggle.
  }

  void streaming_will_start() {
    // Nothing to do
  }

  void streaming_will_stop() {
    // Nothing to do
  }

  /**
   * @brief Re-execute Sunshine with its original arguments.
   */
  void restart_on_exit() {
    // Haiku has no /proc/self/exe; the team's application image carries the executable path.
    image_info info;
    int32 cookie = 0;
    std::string executable;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
      if (info.type == B_APP_IMAGE) {
        executable = info.name;
        break;
      }
    }
    if (executable.empty()) {
      BOOST_LOG(fatal) << "Couldn't determine the Sunshine executable path for restart"sv;
      return;
    }

    // ASIO doesn't use O_CLOEXEC, so close all inherited descriptors ourselves.
    auto openmax = (int) sysconf(_SC_OPEN_MAX);
    for (int fd = STDERR_FILENO + 1; fd < openmax; fd++) {
      close(fd);
    }

    if (execv(executable.c_str(), lifetime::get_argv()) < 0) {
      BOOST_LOG(fatal) << "execv() failed: "sv << errno;
    }
  }

  void restart() {
    atexit(restart_on_exit);
    lifetime::exit_sunshine(0, true);
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    if (kill(-((pid_t) native_handle), SIGTERM) == 0 || errno == ESRCH) {
      BOOST_LOG(debug) << "Successfully sent SIGTERM to process group: "sv << native_handle;
      return true;
    }

    BOOST_LOG(warning) << "Unable to send SIGTERM to process group ["sv << native_handle << "]: "sv << errno;
    return false;
  }

  bool process_group_running(std::uintptr_t native_handle) {
    return waitpid(-((pid_t) native_handle), nullptr, WNOHANG) >= 0;
  }

  namespace {
    /**
     * @brief Destination address storage for sendmsg().
     */
    struct target_t {
      sockaddr_storage storage {};
      socklen_t length = 0;
    };

    /**
     * @brief Convert a Boost address and port into a socket address.
     */
    target_t to_target(const boost::asio::ip::address &address, uint16_t port) {
      target_t target;
      if (address.is_v6()) {
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(&target.storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        v6->sin6_scope_id = address.to_v6().scope_id();
        auto bytes = address.to_v6().to_bytes();
        memcpy(&v6->sin6_addr, bytes.data(), sizeof(v6->sin6_addr));
        target.length = sizeof(sockaddr_in6);
      } else {
        auto *v4 = reinterpret_cast<sockaddr_in *>(&target.storage);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        auto bytes = address.to_v4().to_bytes();
        memcpy(&v4->sin_addr, bytes.data(), sizeof(v4->sin_addr));
        target.length = sizeof(sockaddr_in);
      }
      return target;
    }

    /**
     * @brief Send one datagram, waiting for buffer space if the socket is full.
     *
     * @return `true` once the datagram was handed to the kernel.
     */
    bool send_datagram(int sockfd, msghdr &msg) {
      while (true) {
        if (sendmsg(sockfd, &msg, 0) >= 0) {
          return true;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
          pollfd pfd {sockfd, POLLOUT, 0};
          if (poll(&pfd, 1, -1) != 1) {
            BOOST_LOG(warning) << "poll() failed: "sv << errno;
            return false;
          }
          continue;
        }

        BOOST_LOG(verbose) << "sendmsg() failed: "sv << errno;
        return false;
      }
    }
  }  // namespace

  // Haiku has neither sendmmsg() nor UDP segmentation offload, and does not accept a source
  // address as ancillary data (no IP_PKTINFO / IP_SENDSRCADDR). Each packet is a separate
  // sendmsg(); the routing table picks the source address, which is correct on hosts with a
  // single active network interface.

  bool send_batch(const batched_send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    auto target = to_target(send_info.target_address, send_info.target_port);

    for (size_t i = 0; i < send_info.block_count; i++) {
      std::array<iovec, 2> iovs {};
      int iovlen = 0;

      const auto block = send_info.block_offset + i;
      if (send_info.headers) {
        iovs[iovlen].iov_base = (void *) &send_info.headers[block * send_info.header_size];
        iovs[iovlen].iov_len = send_info.header_size;
        iovlen++;
      }

      auto payload_desc = send_info.buffer_for_payload_offset(block * send_info.payload_size);
      iovs[iovlen].iov_base = (void *) payload_desc.buffer;
      iovs[iovlen].iov_len = send_info.payload_size;
      iovlen++;

      msghdr msg {};
      msg.msg_name = &target.storage;
      msg.msg_namelen = target.length;
      msg.msg_iov = iovs.data();
      msg.msg_iovlen = iovlen;

      if (!send_datagram(sockfd, msg)) {
        return false;
      }
    }

    return true;
  }

  bool send(send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    auto target = to_target(send_info.target_address, send_info.target_port);

    std::array<iovec, 2> iovs {};
    int iovlen = 0;
    if (send_info.header) {
      iovs[iovlen].iov_base = (void *) send_info.header;
      iovs[iovlen].iov_len = send_info.header_size;
      iovlen++;
    }
    iovs[iovlen].iov_base = (void *) send_info.payload;
    iovs[iovlen].iov_len = send_info.payload_size;
    iovlen++;

    msghdr msg {};
    msg.msg_name = &target.storage;
    msg.msg_namelen = target.length;
    msg.msg_iov = iovs.data();
    msg.msg_iovlen = iovlen;

    if (!send_datagram(sockfd, msg)) {
      BOOST_LOG(warning) << "sendmsg() failed: "sv << errno;
      return false;
    }
    return true;
  }

  // Track QoS users so TOS is only reset once the last client disconnects.
  static std::atomic qos_ref_count = 0;

  /**
   * @brief Socket QoS state that is reset when the last stream ends.
   */
  class qos_t: public deinit_t {
  public:
    qos_t(int sockfd, std::vector<std::tuple<int, int, int>> options):
        sockfd(sockfd),
        options(std::move(options)) {
      qos_ref_count++;
    }

    ~qos_t() override {
      if (--qos_ref_count == 0) {
        for (const auto &[level, option, reset_val] : options) {
          if (setsockopt(sockfd, level, option, &reset_val, sizeof(reset_val)) < 0) {
            BOOST_LOG(warning) << "Failed to reset option: "sv << errno;
          }
        }
      }
    }

  private:
    int sockfd;
    std::vector<std::tuple<int, int, int>> options;
  };

  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    auto sockfd = (int) native_socket;
    std::vector<std::tuple<int, int, int>> reset_options;

    // Haiku supports IP_TOS for IPv4. It has no IPV6_TCLASS and no SO_PRIORITY.
    if (dscp_tagging && !(address.is_v6() && !address.to_v6().is_v4_mapped())) {
      int dscp = 0;
      switch (data_type) {
        case qos_data_type_e::video:
          dscp = 40;
          break;
        case qos_data_type_e::audio:
          dscp = 48;
          break;
        default:
          BOOST_LOG(error) << "Unknown traffic type: "sv << std::to_underlying(data_type);
          break;
      }

      if (dscp) {
        int tos = dscp << 2;
        if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0) {
          reset_options.emplace_back(IPPROTO_IP, IP_TOS, 0);
        } else {
          BOOST_LOG(debug) << "IP_TOS is not supported here: "sv << errno;
        }
      }
    }

    return std::make_unique<qos_t>(sockfd, std::move(reset_options));
  }

  std::string get_host_name() {
    try {
      return boost::asio::ip::host_name();
    } catch (boost::system::system_error &err) {
      BOOST_LOG(error) << "Failed to get hostname: "sv << err.what();
      return "Sunshine"s;
    }
  }

  std::unique_ptr<deinit_t> init() {
    if (!haiku::start_application()) {
      BOOST_LOG(error) << "Couldn't start the Haiku application; screen capture and audio will not work"sv;
    }

    if (!haiku::input_addon_running()) {
      BOOST_LOG(warning) << "The libvirtualhid input_server add-on is not running, so clients cannot control this computer. "sv
                         << "Install virtualhid_input to ~/config/non-packaged/add-ons/input_server/devices/ and restart "sv
                         << "input_server (or reboot). Input starts working as soon as it is loaded."sv;
    }

    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
        haiku::stop_application();
      }
    };

    return std::make_unique<deinit_t>();
  }

  /**
   * @brief Sleep-based timer; Haiku's snooze() already has microsecond resolution.
   */
  class haiku_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    explicit operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<haiku_high_precision_timer>();
  }

  std::string resolve_render_device() {
    // FFmpeg's Vulkan device selector accepts a device index or a name substring; an empty string
    // picks the first device that supports video encoding.
    return config::video.adapter_name;
  }

  bool has_elevated_privileges(bool /*all_caps*/) {
    // Haiku does not separate administrator and user privileges for desktop programs.
    return false;
  }

  void drop_elevated_privileges(bool /*all_caps*/) {
    // Nothing to drop on Haiku.
  }

}  // namespace platf
