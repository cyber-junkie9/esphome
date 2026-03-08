#include "esphome/core/defines.h"
#ifdef USE_NETWORK
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "udp_component.h"

namespace esphome::udp {

static const char *const TAG = "udp";

void UDPComponent::setup() {
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
  for (const auto &address : this->addresses_) {
    struct sockaddr_storage saddr {};

#ifdef USE_UDP_IPV6
    if (strchr(address, ':') != nullptr) {
      auto *addr6 = (struct sockaddr_in6 *) &saddr;
      addr6->sin6_family = AF_INET6;
      addr6->sin6_port = htons(this->broadcast_port_);
      inet_pton(AF_INET6, address, &addr6->sin6_addr);
      addr6->sin6_scope_id = 0;
    } else
#endif
    {
      auto *addr4 = (struct sockaddr_in *) &saddr;
      addr4->sin_family = AF_INET;
      addr4->sin_port = htons(this->broadcast_port_);
      inet_aton(address, &addr4->sin_addr);
    }

    this->sockaddrs_.push_back(saddr);
  }

#ifdef USE_UDP_IPV6
  int socket_family = AF_INET6;
  bool has_ipv6 = false;
  for (const auto &address : this->addresses_) {
    if (strchr(address, ':') != nullptr) {
      has_ipv6 = true;
      break;
    }
  }
  if (!has_ipv6) {
    socket_family = AF_INET;
  }
#else
  int socket_family = AF_INET;
#endif

  // set up broadcast socket
  if (this->should_broadcast_) {
    this->broadcast_socket_ = socket::socket(socket_family, SOCK_DGRAM, IPPROTO_IP);
    if (this->broadcast_socket_ == nullptr) {
      this->status_set_error(LOG_STR("Could not create socket"));
      this->mark_failed();
      return;
    }
    int enable = 1;
    auto err = this->broadcast_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
    if (err != 0) {
      this->status_set_warning(LOG_STR("Socket unable to set reuseaddr"));
      // we can still continue
    }

#ifdef USE_UDP_IPV6
    if (socket_family == AF_INET6) {
      int v6only = 0;
      err = this->broadcast_socket_->setsockopt(IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
      if (err != 0) {
        this->status_set_warning(LOG_STR("Socket unable to set IPV6_V6ONLY"));
      }
    } else
#endif
    {
      err = this->broadcast_socket_->setsockopt(SOL_SOCKET, SO_BROADCAST, &enable, sizeof(int));
      if (err != 0) {
        this->status_set_warning(LOG_STR("Socket unable to set broadcast"));
      }
    }
  }

  // create listening socket if we either want to subscribe to providers, or need to listen
  // for ping key broadcasts.
  if (this->should_listen_) {
    this->listen_socket_ = socket::socket(socket_family, SOCK_DGRAM, IPPROTO_IP);
    if (this->listen_socket_ == nullptr) {
      this->status_set_error(LOG_STR("Could not create socket"));
      this->mark_failed();
      return;
    }
    auto err = this->listen_socket_->setblocking(false);
    if (err < 0) {
      ESP_LOGE(TAG, "Unable to set nonblocking: errno %d", errno);
      this->status_set_error(LOG_STR("Unable to set nonblocking"));
      this->mark_failed();
      return;
    }
    int enable = 1;
    err = this->listen_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (err != 0) {
      this->status_set_warning(LOG_STR("Socket unable to set reuseaddr"));
      // we can still continue
    }

#ifdef USE_UDP_IPV6
    if (socket_family == AF_INET6) {
      struct sockaddr_in6 server {};
      server.sin6_family = AF_INET6;
      server.sin6_addr = in6addr_any;
      server.sin6_port = htons(this->listen_port_);

      if (this->listen_address_.has_value()) {
        char addr_buf[network::IP_ADDRESS_BUFFER_SIZE];
        this->listen_address_.value().str_to(addr_buf);

        // Check if it's IPv6 multicast
        if (strchr(addr_buf, ':') != nullptr) {
          struct ipv6_mreq mreq6 = {};
          inet_pton(AF_INET6, addr_buf, &mreq6.ipv6mr_multiaddr);
          mreq6.ipv6mr_interface = 0;  // Use default interface
          memcpy(&server.sin6_addr, &mreq6.ipv6mr_multiaddr, sizeof(struct in6_addr));
          ESP_LOGD(TAG, "Join IPv6 multicast %s", addr_buf);
          err = this->listen_socket_->setsockopt(IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
          if (err < 0) {
            ESP_LOGE(TAG, "Failed to set IPV6_JOIN_GROUP. Error %d", errno);
            this->status_set_error(LOG_STR("Failed to set IPV6_JOIN_GROUP"));
            this->mark_failed();
            return;
          }
        }
      }

      err = this->listen_socket_->bind((struct sockaddr *) &server, sizeof(server));
      if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        this->status_set_error(LOG_STR("Unable to bind socket"));
        this->mark_failed();
        return;
      }
    } else
#endif
    {
      struct sockaddr_in server {};

      server.sin_family = AF_INET;
      server.sin_addr.s_addr = ESPHOME_INADDR_ANY;
      server.sin_port = htons(this->listen_port_);

      if (this->listen_address_.has_value()) {
        // Only 16 bytes needed for IPv4, but use standard size for consistency
        char addr_buf[network::IP_ADDRESS_BUFFER_SIZE];
        this->listen_address_.value().str_to(addr_buf);
        struct ip_mreq imreq = {};
        imreq.imr_interface.s_addr = ESPHOME_INADDR_ANY;
        inet_aton(addr_buf, &imreq.imr_multiaddr);
        server.sin_addr.s_addr = imreq.imr_multiaddr.s_addr;
        ESP_LOGD(TAG, "Join multicast %s", addr_buf);
        err = this->listen_socket_->setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq));
        if (err < 0) {
          ESP_LOGE(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
          this->status_set_error(LOG_STR("Failed to set IP_ADD_MEMBERSHIP"));
          this->mark_failed();
          return;
        }
      }

      err = this->listen_socket_->bind((struct sockaddr *) &server, sizeof(server));
      if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        this->status_set_error(LOG_STR("Unable to bind socket"));
        this->mark_failed();
        return;
      }
    }
  }
#endif
#ifdef USE_SOCKET_IMPL_LWIP_TCP
  // 8266 and RP2040 `Duino
  for (const auto &address : this->addresses_) {
    auto ipaddr = IPAddress();
    ipaddr.fromString(address);
    this->ipaddrs_.push_back(ipaddr);
  }
  if (this->should_listen_)
    this->udp_client_.begin(this->listen_port_);
#endif
}

void UDPComponent::loop() {
  if (this->should_listen_) {
    std::array<uint8_t, MAX_PACKET_SIZE> buf;
    for (;;) {
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
      auto len = this->listen_socket_->read(buf.data(), buf.size());
#endif
#ifdef USE_SOCKET_IMPL_LWIP_TCP
      auto len = this->udp_client_.parsePacket();
      if (len > 0)
        len = this->udp_client_.read(buf.data(), buf.size());
#endif
      if (len <= 0)
        break;
      size_t packet_len = static_cast<size_t>(len);
      ESP_LOGV(TAG, "Received packet of length %zu", packet_len);
      this->packet_listeners_.call(std::span<const uint8_t>(buf.data(), packet_len));
    }
  }
}

void UDPComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "UDP:\n"
                "  Listen Port: %u\n"
                "  Broadcast Port: %u",
                this->listen_port_, this->broadcast_port_);
  for (const char *address : this->addresses_)
    ESP_LOGCONFIG(TAG, "  Address: %s", address);
  if (this->listen_address_.has_value()) {
    char addr_buf[network::IP_ADDRESS_BUFFER_SIZE];
    ESP_LOGCONFIG(TAG, "  Listen address: %s", this->listen_address_.value().str_to(addr_buf));
  }
#ifdef USE_UDP_IPV6
  ESP_LOGCONFIG(TAG, "  IPv6: Enabled");
#endif
  ESP_LOGCONFIG(TAG,
                "  Broadcasting: %s\n"
                "  Listening: %s",
                YESNO(this->should_broadcast_), YESNO(this->should_listen_));
}

void UDPComponent::send_packet(const uint8_t *data, size_t size) {
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
  for (const auto &saddr : this->sockaddrs_) {
    socklen_t addr_len;
#ifdef USE_UDP_IPV6
    if (((struct sockaddr *)&saddr)->sa_family == AF_INET6) {
      addr_len = sizeof(struct sockaddr_in6);
    } else
#endif
    {
      addr_len = sizeof(struct sockaddr_in);
    }
    auto result = this->broadcast_socket_->sendto(data, size, 0, (struct sockaddr *)&saddr, addr_len);
    if (result < 0)
      ESP_LOGW(TAG, "sendto() error %d", errno);
  }
#endif
#ifdef USE_SOCKET_IMPL_LWIP_TCP
  auto iface = IPAddress(0, 0, 0, 0);
  for (const auto &saddr : this->ipaddrs_) {
    if (this->udp_client_.beginPacketMulticast(saddr, this->broadcast_port_, iface, 128) != 0) {
      this->udp_client_.write(data, size);
      auto result = this->udp_client_.endPacket();
      if (result == 0)
        ESP_LOGW(TAG, "udp.write() error");
    }
  }
#endif
}
}  // namespace esphome::udp

#endif
