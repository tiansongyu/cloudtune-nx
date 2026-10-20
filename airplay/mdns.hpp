/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace cloudtune::airplay {
/* Minimal IPv4 DNS-SD advertiser for one _raop._tcp service. No threads.
 * start/tick/stop must all run on the receiver's network thread.
 * IPv4 address is in network byte order (the same as in_addr.s_addr). */
class MdnsAdvertiser {
  public:
    MdnsAdvertiser();
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser &) = delete;
    MdnsAdvertiser &operator=(const MdnsAdvertiser &) = delete;
    bool start(const std::string &name, uint16_t port, const std::string &deviceId,
               uint32_t ipv4NetworkOrder);
    void tick();
    void stop();
    int fd() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace cloudtune::airplay
