/**
 * @file src/platform/haiku/mdns.cpp
 * @brief Minimal multicast DNS service announcer used for Moonlight discovery on Haiku.
 */

// standard includes
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>

// platform includes
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// local includes
#include "mdns.h"

namespace platf::mdns {
  namespace {
    constexpr std::uint16_t MDNS_PORT = 5353;
    constexpr const char *MDNS_GROUP = "224.0.0.251";
    constexpr std::uint16_t CLASS_IN = 1;
    constexpr std::uint16_t CACHE_FLUSH = 0x8000;  ///< RFC 6762 10.2, for unique records.
    constexpr std::uint16_t QU_BIT = 0x8000;  ///< RFC 6762 5.4, unicast response requested.

    // RFC 6762 section 10: 120 s for records containing a host name, 75 min for the rest.
    constexpr std::uint32_t TTL_HOST = 120;
    constexpr std::uint32_t TTL_OTHER = 4500;
    constexpr std::uint32_t TTL_LEGACY_MAX = 10;

    /**
     * @brief Re-announcement schedule. Startup follows RFC 6762 8.3 (at least two announcements
     *        one second apart); the periodic announcements afterwards keep discovery working even
     *        where incoming multicast queries are not delivered.
     */
    constexpr std::chrono::seconds ANNOUNCE_DELAYS[] = {std::chrono::seconds {1}, std::chrono::seconds {2}, std::chrono::seconds {4}};
    constexpr std::chrono::seconds ANNOUNCE_PERIOD {60};

    std::string lower(std::string value) {
      std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    /**
     * @brief Writes DNS wire format.
     */
    class writer_t {
    public:
      void u8(std::uint8_t value) {
        data.push_back(value);
      }

      void u16(std::uint16_t value) {
        data.push_back(static_cast<std::uint8_t>(value >> 8));
        data.push_back(static_cast<std::uint8_t>(value));
      }

      void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value >> 16));
        u16(static_cast<std::uint16_t>(value));
      }

      /**
       * @brief Write a name as a sequence of labels (no compression, which RFC 1035 permits).
       *
       * Labels are split at dots; each must be 1 to 63 bytes.
       */
      void name(const std::string &dotted) {
        std::size_t start = 0;
        while (start < dotted.size()) {
          auto end = dotted.find('.', start);
          if (end == std::string::npos) {
            end = dotted.size();
          }
          const auto length = std::min<std::size_t>(end - start, 63);
          if (length > 0) {
            u8(static_cast<std::uint8_t>(length));
            data.insert(data.end(), dotted.begin() + static_cast<std::ptrdiff_t>(start), dotted.begin() + static_cast<std::ptrdiff_t>(start + length));
          }
          start = end + 1;
        }
        u8(0);
      }

      /**
       * @brief Reserve the RDLENGTH field; returns its offset for patch_length().
       */
      std::size_t begin_rdata() {
        const auto offset = data.size();
        u16(0);
        return offset;
      }

      void end_rdata(std::size_t offset) {
        const auto length = data.size() - offset - 2;
        data[offset] = static_cast<std::uint8_t>(length >> 8);
        data[offset + 1] = static_cast<std::uint8_t>(length);
      }

      std::vector<std::uint8_t> data;
    };

    /**
     * @brief Read a possibly compressed name starting at `offset`.
     *
     * @return `false` on malformed input (bad pointers, loops, overruns).
     */
    bool read_name(std::span<const std::uint8_t> packet, std::size_t &offset, std::string &out) {
      out.clear();
      std::size_t position = offset;
      bool jumped = false;
      int hops = 0;

      while (true) {
        if (position >= packet.size()) {
          return false;
        }
        const std::uint8_t length = packet[position];

        if ((length & 0xC0) == 0xC0) {
          if (position + 1 >= packet.size() || ++hops > 16) {
            return false;
          }
          const std::size_t target = (static_cast<std::size_t>(length & 0x3F) << 8) | packet[position + 1];
          if (!jumped) {
            offset = position + 2;
          }
          jumped = true;
          if (target >= packet.size()) {
            return false;
          }
          position = target;
          continue;
        }
        if ((length & 0xC0) != 0) {
          return false;  // reserved label types
        }

        position++;
        if (length == 0) {
          break;
        }
        if (position + length > packet.size() || out.size() + length + 1 > 255) {
          return false;
        }
        if (!out.empty()) {
          out.push_back('.');
        }
        out.append(reinterpret_cast<const char *>(&packet[position]), length);
        position += length;
      }

      if (!jumped) {
        offset = position;
      }
      out = lower(out);
      return true;
    }

    std::uint16_t read_u16(std::span<const std::uint8_t> packet, std::size_t offset) {
      return static_cast<std::uint16_t>((packet[offset] << 8) | packet[offset + 1]);
    }

    /**
     * @brief Names used by the service, all lower-case for comparison.
     */
    struct names_t {
      std::string service_type;  ///< _nvstream._tcp.local
      std::string instance;  ///< <instance>._nvstream._tcp.local
      std::string host;  ///< <host>.local
      std::string enumeration;  ///< _services._dns-sd._udp.local
    };

    names_t make_names(const service_t &service) {
      names_t names;
      names.service_type = service.type + ".local";
      names.instance = service.instance + "." + names.service_type;
      names.host = service.host + ".local";
      names.enumeration = "_services._dns-sd._udp.local";
      return names;
    }

    bool matches(std::uint16_t asked, std::uint16_t type) {
      return asked == type || asked == TYPE_ANY;
    }
  }  // namespace

  bool parse_query(std::span<const std::uint8_t> packet, std::uint16_t &id, std::vector<question_t> &questions) {
    questions.clear();
    if (packet.size() < 12) {
      return false;
    }

    id = read_u16(packet, 0);
    const auto flags = read_u16(packet, 2);
    if ((flags & 0x8000) != 0) {
      return false;  // a response, not a query
    }
    if (((flags >> 11) & 0xF) != 0) {
      return false;  // only standard queries (opcode 0)
    }

    const auto count = read_u16(packet, 4);
    std::size_t offset = 12;
    for (std::uint16_t i = 0; i < count; i++) {
      question_t question;
      if (!read_name(packet, offset, question.name) || offset + 4 > packet.size()) {
        return false;
      }
      question.type = read_u16(packet, offset);
      const auto qclass = read_u16(packet, offset + 2);
      offset += 4;

      question.unicast_response = (qclass & QU_BIT) != 0;
      if ((qclass & ~QU_BIT) != CLASS_IN && (qclass & ~QU_BIT) != TYPE_ANY) {
        continue;
      }
      questions.push_back(std::move(question));
    }
    return true;
  }

  std::vector<std::uint8_t> build_response(const service_t &service, const std::vector<std::uint32_t> &addresses, const std::vector<question_t> &questions, std::uint16_t id, int ttl_scale, bool legacy_unicast) {
    const auto names = make_names(service);
    const auto ttl = [&](std::uint32_t value) {
      value *= static_cast<std::uint32_t>(ttl_scale);
      return legacy_unicast ? std::min(value, TTL_LEGACY_MAX) : value;
    };
    // Legacy unicast responses must not set the cache-flush bit (RFC 6762 6.7).
    const std::uint16_t unique_class = legacy_unicast ? CLASS_IN : static_cast<std::uint16_t>(CLASS_IN | CACHE_FLUSH);

    bool want_enumeration = false;
    bool want_ptr = false;
    bool want_srv = false;
    bool want_txt = false;
    bool want_a = false;
    bool want_aaaa = false;

    if (questions.empty()) {
      // Unsolicited announcement: everything.
      want_ptr = want_srv = want_txt = want_a = true;
    }
    for (const auto &q : questions) {
      if (q.name == names.enumeration && matches(q.type, TYPE_PTR)) {
        want_enumeration = true;
      }
      if (q.name == names.service_type && matches(q.type, TYPE_PTR)) {
        want_ptr = true;
      }
      if (q.name == names.instance) {
        want_srv |= matches(q.type, TYPE_SRV);
        want_txt |= matches(q.type, TYPE_TXT);
      }
      if (q.name == names.host) {
        want_a |= matches(q.type, TYPE_A);
        want_aaaa |= matches(q.type, TYPE_AAAA);
      }
    }

    if (!(want_enumeration || want_ptr || want_srv || want_txt || want_a || want_aaaa)) {
      return {};
    }

    // Answers first, then the records a resolver will need next (RFC 6763 section 12).
    struct pending_t {
      int kind;  ///< 0 enum PTR, 1 PTR, 2 SRV, 3 TXT, 4 A
    };

    std::vector<pending_t> answers;
    std::vector<pending_t> additionals;
    auto add_answer = [&](int kind) {
      answers.push_back({kind});
    };

    if (want_enumeration) {
      add_answer(0);
    }
    if (want_ptr) {
      add_answer(1);
    }
    if (want_srv) {
      add_answer(2);
    }
    if (want_txt) {
      add_answer(3);
    }
    if (want_a) {
      add_answer(4);
    }

    // Additional records only for real queries; announcements already carry everything.
    if (!questions.empty()) {
      auto has = [&](int kind) {
        return std::ranges::any_of(answers, [kind](const pending_t &p) {
          return p.kind == kind;
        });
      };
      if (want_ptr) {
        if (!has(2)) {
          additionals.push_back({2});
        }
        if (!has(3)) {
          additionals.push_back({3});
        }
      }
      if ((want_ptr || want_srv || want_aaaa) && !has(4)) {
        additionals.push_back({4});  // A records (also the negative answer for AAAA, RFC 6762 6.2)
      }
    }

    auto record_count = [&](const std::vector<pending_t> &list) {
      std::uint16_t count = 0;
      for (const auto &p : list) {
        count += p.kind == 4 ? static_cast<std::uint16_t>(addresses.size()) : 1;
      }
      return count;
    };

    writer_t w;
    w.u16(legacy_unicast ? id : 0);
    w.u16(0x8400);  // response, authoritative answer
    w.u16(legacy_unicast ? static_cast<std::uint16_t>(questions.size()) : 0);
    w.u16(record_count(answers));
    w.u16(0);
    w.u16(record_count(additionals));

    if (legacy_unicast) {
      for (const auto &q : questions) {
        w.name(q.name);
        w.u16(q.type);
        w.u16(CLASS_IN);
      }
    }

    auto write_record = [&](const pending_t &p) {
      switch (p.kind) {
        case 0:
          {
            w.name(names.enumeration);
            w.u16(TYPE_PTR);
            w.u16(CLASS_IN);  // shared record
            w.u32(ttl(TTL_OTHER));
            auto rd = w.begin_rdata();
            w.name(names.service_type);
            w.end_rdata(rd);
            break;
          }
        case 1:
          {
            w.name(names.service_type);
            w.u16(TYPE_PTR);
            w.u16(CLASS_IN);  // shared record
            w.u32(ttl(TTL_OTHER));
            auto rd = w.begin_rdata();
            w.name(names.instance);
            w.end_rdata(rd);
            break;
          }
        case 2:
          {
            w.name(names.instance);
            w.u16(TYPE_SRV);
            w.u16(unique_class);
            w.u32(ttl(TTL_HOST));
            auto rd = w.begin_rdata();
            w.u16(0);  // priority
            w.u16(0);  // weight
            w.u16(service.port);
            w.name(names.host);
            w.end_rdata(rd);
            break;
          }
        case 3:
          {
            w.name(names.instance);
            w.u16(TYPE_TXT);
            w.u16(unique_class);
            w.u32(ttl(TTL_OTHER));
            auto rd = w.begin_rdata();
            w.u8(0);  // RFC 6763 6.1: an empty TXT record is a single zero-length string
            w.end_rdata(rd);
            break;
          }
        case 4:
          for (const auto address : addresses) {
            w.name(names.host);
            w.u16(TYPE_A);
            w.u16(unique_class);
            w.u32(ttl(TTL_HOST));
            auto rd = w.begin_rdata();
            // `address` is in network byte order; copy its bytes as they are.
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
            w.u8(bytes[0]);
            w.u8(bytes[1]);
            w.u8(bytes[2]);
            w.u8(bytes[3]);
            w.end_rdata(rd);
          }
          break;
        default:
          break;
      }
    };

    for (const auto &p : answers) {
      write_record(p);
    }
    for (const auto &p : additionals) {
      write_record(p);
    }

    // Nothing to say (e.g. an A query while the host has no address yet). An AAAA query still
    // gets the A records as additional data, telling the client to use IPv4.
    if (record_count(answers) == 0 && record_count(additionals) == 0) {
      return {};
    }
    return w.data;
  }

  responder_t::responder_t(service_t service, std::function<std::vector<std::uint32_t>()> address_provider):
      service {std::move(service)},
      address_provider {std::move(address_provider)} {
    socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
      return;
    }

    int on = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    sockaddr_in bind_address {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(MDNS_PORT);
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_fd, reinterpret_cast<sockaddr *>(&bind_address), sizeof(bind_address)) != 0) {
      ::close(socket_fd);
      socket_fd = -1;
      return;
    }

    // Joining the group may fail on hosts without a multicast route; announcements still go out.
    ip_mreq membership {};
    inet_pton(AF_INET, MDNS_GROUP, &membership.imr_multiaddr);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));

    // BSD-derived stacks (including Haiku) take these options as a single byte.
    unsigned char ttl = 255;
    setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    unsigned char loop = 1;
    setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    thread = std::thread(&responder_t::run, this);
  }

  responder_t::~responder_t() {
    stop = true;
    if (thread.joinable()) {
      thread.join();
    }
    if (socket_fd >= 0) {
      // Goodbye: the same records with TTL 0 so clients drop them right away.
      send_multicast(build_response(service, address_provider(), {}, 0, 0, false));
      ::close(socket_fd);
    }
  }

  bool responder_t::running() const {
    return socket_fd >= 0;
  }

  void responder_t::send_multicast(const std::vector<std::uint8_t> &packet) const {
    if (packet.empty() || socket_fd < 0) {
      return;
    }
    sockaddr_in group {};
    group.sin_family = AF_INET;
    group.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
    ::sendto(socket_fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&group), sizeof(group));
  }

  void responder_t::run() {
    using clock = std::chrono::steady_clock;

    std::size_t announcements = 0;
    auto next_announcement = clock::now();
    std::vector<std::uint8_t> buffer(9000);
    std::vector<question_t> questions;

    while (!stop) {
      const auto now = clock::now();
      if (now >= next_announcement) {
        send_multicast(build_response(service, address_provider(), {}, 0, 1, false));
        const auto delay = announcements < std::size(ANNOUNCE_DELAYS) ? ANNOUNCE_DELAYS[announcements] : ANNOUNCE_PERIOD;
        announcements++;
        next_announcement = now + delay;
      }

      pollfd pfd {socket_fd, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 250);
      if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
        continue;
      }

      sockaddr_in from {};
      socklen_t from_length = sizeof(from);
      const auto received = ::recvfrom(socket_fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&from), &from_length);
      if (received <= 0) {
        continue;
      }

      std::uint16_t id = 0;
      if (!parse_query({buffer.data(), static_cast<std::size_t>(received)}, id, questions) || questions.empty()) {
        continue;
      }

      // RFC 6762 6.7: a query from a port other than 5353 comes from a simple resolver that
      // expects a conventional unicast DNS reply.
      const bool legacy = ntohs(from.sin_port) != MDNS_PORT;
      const bool unicast = legacy || std::ranges::all_of(questions, [](const question_t &q) {
                             return q.unicast_response;
                           });

      auto response = build_response(service, address_provider(), questions, id, 1, legacy);
      if (response.empty()) {
        continue;
      }

      if (unicast) {
        ::sendto(socket_fd, response.data(), response.size(), 0, reinterpret_cast<const sockaddr *>(&from), from_length);
      } else {
        send_multicast(response);
      }
    }
  }

}  // namespace platf::mdns
