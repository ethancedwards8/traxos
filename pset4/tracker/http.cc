#include "tracker/http.hh"

#include "tracker/state.hh"

#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

template <typename T>
std::optional<T> parse_number(std::string_view value) {
    T result{};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc() || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

template <size_t N>
bool copy_exact(char (&field)[N], std::string_view value) {
    if (value.size() != N) {
        return false;
    }
    memcpy(field, value.data(), N);
    return true;
}

template <size_t N>
std::string hex_bytes(const std::array<char, N>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(N * 2);
    for (char byte : bytes) {
        auto value = static_cast<unsigned char>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0xf]);
    }
    return out;
}

std::optional<uint32_t> parse_ipv4(std::string_view value) {
    std::string dotted(value);
    dotted += "."; // hack

    uint32_t ip = 0;
    size_t start = 0;

    for (int i = 0; i != 4; ++i) {
        size_t dot = dotted.find('.', start);

        auto part = parse_number<uint32_t>(std::string_view(dotted).substr(start, dot - start));
        if (!part || *part > 255) {
            return std::nullopt;
        }

        ip = (ip << 8) | *part;
        start = dot + 1;
    }

    if (start != dotted.size()) {
        return std::nullopt;
    }

    return ip;
}

std::string format_ipv4(uint32_t ip) {
    return std::format("{}.{}.{}.{}",
                       (ip >> 24) & 0xff,
                       (ip >> 16) & 0xff,
                       (ip >> 8) & 0xff,
                       ip & 0xff);
}

// https://stackoverflow.com/questions/20472072/c-socket-get-ip-address-from-filedescriptor-returned-from-accept
std::optional<uint32_t> peer_ipv4(cotamer::fd& cfd) {
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    if (getpeername(cfd.fileno(), reinterpret_cast<sockaddr*>(&addr), &addrlen) < 0
        || addr.sin_family != AF_INET) {
        return std::nullopt;
    }
    return ntohl(addr.sin_addr.s_addr);
}

void parse_announce_request(cotamer::http_message& req,
                            bt_tracker_announce_request& bt_req,
                            std::string& failure) {
    for (auto it = req.search_param_begin(); it != req.search_param_end(); it++) {
        std::string_view name(it.name());
        // live laugh love c++
        std::string value(it.value());

        std::cout << name << " and " << value << std::endl;

        if (name == "info_hash") {

            if (!copy_exact(bt_req.info_hash, value)) {
                failure = "invalid info_hash";
            }

        } else if (name == "peer_id") {

            if (!copy_exact(bt_req.peer_id, value)) {
                failure = "invalid peer_id";
            }

        } else if (name == "ip") {

            auto ip = parse_ipv4(value);
            if (!ip) {
                failure = "invalid ip";
            } else {
                bt_req.ip = *ip;
            }

        } else if (name == "port") {

            auto port = parse_number<uint16_t>(value);
            if (!port) {
                failure = "invalid port";
            } else {
                bt_req.port = *port;
            }

        } else if (name == "uploaded") {

            auto uploaded = parse_number<uint64_t>(value);
            if (!uploaded) {
                failure = "invalid uploaded";
            } else {
                bt_req.uploaded = *uploaded;
            }

        } else if (name == "downloaded") {

            auto downloaded = parse_number<uint64_t>(value);
            if (!downloaded) {
                failure = "invalid downloaded";
            } else {
                bt_req.downloaded = *downloaded;
            }

        } else if (name == "left") {

            auto left = parse_number<uint64_t>(value);
            if (!left) {
                failure = "invalid left";
            } else {
                bt_req.left = *left;
            }

        } else if (name == "event") {

            if (value == "started") {

                bt_req.event = started;

            } else if (value == "stopped") {

                bt_req.event = stopped;

            } else if (value == "completed") {

                bt_req.event = completed;

            } else {

                failure = "invalid event";

            }

        } else if (name == "numwant") {

            auto numwant = parse_number<int32_t>(value);
            if (!numwant) {
                failure = "invalid numwant";
            } else {
                bt_req.numwant = *numwant;
            }

        } else {

        }
    }
}

std::string tracker_success_response(const bt_tracker_state& state,
                                     const bt_tracker_announce_request& bt_req) {
    bt_info_hash info_hash = make_info_hash(bt_req.info_hash);
    auto torrent = state.torrents.find(info_hash);

    std::string body = "d8:intervali100e5:peersl";
    if (torrent != state.torrents.end()) {
        int32_t count = 0;
        for (auto& [peer_id, peer] : torrent->second) {
            // own peer doesn't need to be in peer list
            if (peer_id == make_peer_id(bt_req.peer_id)) {
                continue;
            }
            if (count == bt_req.numwant) {
                break;
            }

            std::string ip = format_ipv4(peer.ip);
            body += std::format("d2:ip{}:{}4:porti{}ee", ip.size(), ip, peer.port);
            ++count;
        }
    }
    body += "ee";
    return body;
}

std::string tracker_debug_response(const bt_tracker_state& state) {
    std::string body;
    body += std::format("torrents: {}\n", state.torrents.size());
    for (auto& [info_hash, peers] : state.torrents) {
        body += std::format("info_hash {} peers: {}\n", hex_bytes(info_hash), peers.size());
        for (auto& [peer_id, peer] : peers) {
            body += std::format("  peer_id {} ip {} port {} left {} complete {}\n",
                                hex_bytes(peer_id),
                                format_ipv4(peer.ip),
                                peer.port,
                                peer.left,
                                peer.complete ? "yes" : "no");
        }
    }
    return body;
}
