/**
 * @file src/platform/haiku/mdns.h
 * @brief Minimal multicast DNS service announcer used for Moonlight discovery on Haiku.
 *
 * Haiku does not run an mDNS daemon by default (HaikuPorts' mdnsresponder has to be started by
 * hand), so Sunshine answers the few queries Moonlight needs itself:
 *
 *   PTR  _nvstream._tcp.local            -> <instance>._nvstream._tcp.local
 *   PTR  _services._dns-sd._udp.local    -> _nvstream._tcp.local
 *   SRV  <instance>._nvstream._tcp.local -> <host>.local:<port>
 *   TXT  <instance>._nvstream._tcp.local -> (empty)
 *   A    <host>.local                    -> every IPv4 address of this machine
 *
 * The responder follows RFC 6762 (multicast DNS) and RFC 6763 (DNS-SD): it announces the
 * service when it starts, answers queries on 224.0.0.251:5353, honours unicast-response and
 * legacy unicast queries, and sends goodbye packets when it stops. It shares port 5353 with other
 * responders (SO_REUSEADDR/SO_REUSEPORT), so it coexists with an mdnsd started by the user.
 *
 * The code only uses POSIX sockets, which keeps it independent of Haiku releases and testable on
 * any system.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace platf::mdns {

  /**
   * @brief DNS record types used by the responder.
   */
  enum record_type_e : std::uint16_t {
    TYPE_A = 1,
    TYPE_PTR = 12,
    TYPE_TXT = 16,
    TYPE_AAAA = 28,
    TYPE_SRV = 33,
    TYPE_ANY = 255,
  };

  /**
   * @brief Service description advertised by the responder.
   */
  struct service_t {
    std::string instance;  ///< Instance label, e.g. "my-haiku-pc".
    std::string type;  ///< Service type without domain, e.g. "_nvstream._tcp".
    std::string host;  ///< Host label without domain, e.g. "my-haiku-pc".
    std::uint16_t port = 0;  ///< TCP port of the service.
  };

  /**
   * @brief Question parsed from an incoming packet.
   */
  struct question_t {
    std::string name;  ///< Lower-case, dot-separated name without trailing dot.
    std::uint16_t type = 0;
    bool unicast_response = false;  ///< The QU bit was set.
  };

  /**
   * @brief Parse the question section of a DNS message.
   *
   * @param packet Raw DNS message.
   * @param id Receives the message ID.
   * @param questions Receives the parsed questions.
   * @return `false` for malformed packets and responses (only queries are accepted).
   */
  bool parse_query(std::span<const std::uint8_t> packet, std::uint16_t &id, std::vector<question_t> &questions);

  /**
   * @brief Build a DNS response with the records answering `questions`.
   *
   * @param service Advertised service.
   * @param addresses IPv4 addresses (network byte order) for the host's A records.
   * @param questions Questions to answer; an empty list builds an unsolicited announcement.
   * @param id Message ID (0 for multicast responses).
   * @param ttl_scale 1 for normal answers, 0 for goodbye packets.
   * @param legacy_unicast Echo the questions and cap TTLs at 10 s (RFC 6762 section 6.7).
   * @return Encoded message, or an empty vector when there is nothing to answer.
   */
  std::vector<std::uint8_t> build_response(const service_t &service, const std::vector<std::uint32_t> &addresses, const std::vector<question_t> &questions, std::uint16_t id, int ttl_scale, bool legacy_unicast);

  /**
   * @brief Background responder thread.
   */
  class responder_t {
  public:
    /**
     * @brief Start announcing `service`.
     *
     * @param service Service to advertise.
     * @param address_provider Returns the current IPv4 addresses (network byte order); called for
     *        every response so DHCP changes are picked up.
     */
    responder_t(service_t service, std::function<std::vector<std::uint32_t>()> address_provider);

    /**
     * @brief Send goodbye packets and stop the thread.
     */
    ~responder_t();

    responder_t(const responder_t &) = delete;
    responder_t &operator=(const responder_t &) = delete;

    /**
     * @brief Check whether the multicast socket could be opened.
     *
     * @return `true` while the responder is running.
     */
    bool running() const;

  private:
    void run();
    void send_multicast(const std::vector<std::uint8_t> &packet) const;

    service_t service;
    std::function<std::vector<std::uint32_t>()> address_provider;
    int socket_fd = -1;
    std::atomic_bool stop {false};
    std::thread thread;
  };

}  // namespace platf::mdns
