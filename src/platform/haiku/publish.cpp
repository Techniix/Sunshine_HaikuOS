/**
 * @file src/platform/haiku/publish.cpp
 * @brief Moonlight discovery (mDNS/DNS-SD) on Haiku.
 */

// standard includes
#include <memory>
#include <vector>

// platform includes
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

// local includes
#include "mdns.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf::publish {
  namespace {
    /**
     * @brief Current IPv4 addresses of every interface that is up, excluding loopback.
     *
     * @return Addresses in network byte order.
     */
    std::vector<std::uint32_t> ipv4_addresses() {
      std::vector<std::uint32_t> addresses;

      ifaddrs *list = nullptr;
      if (getifaddrs(&list) != 0) {
        return addresses;
      }
      for (auto *entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
          continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
          continue;
        }
        const auto address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr;
        if (address != 0) {
          addresses.push_back(address);
        }
      }
      freeifaddrs(list);
      return addresses;
    }

    /**
     * @brief Keeps the responder alive for as long as Sunshine runs.
     */
    class deinit_t: public ::platf::deinit_t {
    public:
      explicit deinit_t(std::unique_ptr<mdns::responder_t> responder):
          responder {std::move(responder)} {
      }

      ~deinit_t() override {
        responder.reset();
        BOOST_LOG(info) << "Stopped mDNS announcements"sv;
      }

    private:
      std::unique_ptr<mdns::responder_t> responder;
    };
  }  // namespace

  std::unique_ptr<::platf::deinit_t> start() {
    const auto name = net::mdns_instance_name(platf::get_host_name());

    mdns::service_t service;
    service.instance = name;
    service.type = platf::SERVICE_TYPE;
    service.host = name;
    service.port = net::map_port(nvhttp::PORT_HTTP);

    auto responder = std::make_unique<mdns::responder_t>(service, ipv4_addresses);
    if (!responder->running()) {
      BOOST_LOG(error) << "Couldn't open the mDNS socket (UDP 5353); add this computer in Moonlight by its IP address"sv;
      return nullptr;
    }

    BOOST_LOG(info) << "Announcing "sv << name << '.' << platf::SERVICE_TYPE << ".local on port "sv << service.port;
    return std::make_unique<deinit_t>(std::move(responder));
  }
}  // namespace platf::publish
