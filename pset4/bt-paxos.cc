#include <algorithm>
#include "bittorrent.hh"
#include "cotamer/http.hh"
#include "bencode.h"
#include "cotamer/cotamer.hh"
#include <cassert>
#include <charconv>
#include <libgen.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAXLEN 256;

namespace cot = cotamer;
using namespace std::chrono_literals;

// https://stackoverflow.com/questions/9443957/using-sizeof-on-arrays-passed-as-parameters
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

bt_info_hash make_info_hash(const char (&bytes)[20]) {
    bt_info_hash key;
    memcpy(key.data(), bytes, key.size());
    return key;
}

bt_peer_id make_peer_id(const char (&bytes)[20]) {
    bt_peer_id key;
    memcpy(key.data(), bytes, key.size());
    return key;
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
std::optional<uint32_t> peer_ipv4(cot::fd& cfd) {
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    if (getpeername(cfd.fileno(), reinterpret_cast<sockaddr*>(&addr), &addrlen) < 0
        || addr.sin_family != AF_INET) {
        return std::nullopt;
    }
    return ntohl(addr.sin_addr.s_addr);
}

void parse_announce_request(cot::http_message& req,
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

void apply_announce(bt_tracker_state& state, const bt_tracker_announce_request& bt_req) {
    bt_info_hash info_hash = make_info_hash(bt_req.info_hash);
    bt_peer_id peer_id = make_peer_id(bt_req.peer_id);
    auto& peers = state.torrents[info_hash];

    if (bt_req.event == stopped) {
        peers.erase(peer_id);
        return;
    }

    bt_peer peer;

    peer.peer_id = peer_id;
    peer.ip = bt_req.ip;
    peer.port = bt_req.port;
    peer.uploaded = bt_req.uploaded;
    peer.downloaded = bt_req.downloaded;
    peer.left = bt_req.left;
    peer.complete = bt_req.event == completed || bt_req.left == 0;

    peers[peer_id] = peer;
}

std::string tracker_success_response(const bt_tracker_state& state,
                                     const bt_tracker_announce_request& bt_req) {
    bt_info_hash info_hash = make_info_hash(bt_req.info_hash);
    auto torrent = state.torrents.find(info_hash);

    std::string body = "d8:intervali900e5:peersl";
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

cot::task<> handle_connection(cot::fd cfd, bt_tracker_state& tracker_state) {
    std::optional<uint32_t> connection_ip = peer_ipv4(cfd);
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    do {
        cot::http_message req = co_await hp.receive();
        if (!hp.ok()) {
            break;                                 // peer closed or parse error
        }
        cot::http_message res;

        // https://stackoverflow.com/a/650307
        // interesting idea for later
        // switch (req.url()) {
        //     case "success":
        //         break;
        //     default:
        // }

        // https://stackoverflow.com/questions/48081436/how-you-convert-a-stdstring-view-to-a-const-char
        std::string_view url = req.path();
        std::cout << url << "url read\n" << std::endl;

        bt_tracker_announce_request bt_req{};
        std::string failure;

        if (url == "/announce") {

            parse_announce_request(req, bt_req, failure);
            if (bt_req.ip == 0 && connection_ip) {
                bt_req.ip = *connection_ip;
            }

            if (!failure.empty()) {
                res.status_code(200)
                    .header("Content-Type", "text/plain")
                    .body(failure);
            } else {
                apply_announce(tracker_state, bt_req);
                std::string body = tracker_success_response(tracker_state, bt_req);
                std::cout << "announce response " << body << std::endl;

                res.status_code(200)
                    .header("Content-Type", "text/plain")
                    .body(body);
            }


        } else if (url == "/debug") {
            res.status_code(200)
                .header("Content-Type", "text/plain")
                .body(tracker_debug_response(tracker_state));

        } else {
            res.status_code(404)
                .header("Content-Type", "text/plain")
                .body(std::format("you asked for {}, but probably want /announce instead\n", req.url()));
        }
        co_await hp.send(std::move(res));
    } while (hp.should_keep_alive());
}

cot::task<> run_server() {
    printf("Running the server on 0.0.0.0:9000\n");
    bt_tracker_state tracker_state;
    cot::fd lfd = co_await cot::tcp_listen("0.0.0.0:9000");

    while (true) {
        cot::fd cfd = co_await cot::tcp_accept(lfd);
        handle_connection(std::move(cfd), tracker_state).detach();
    }
}


cot::task<> run_client() {
    cot::fd lfd = co_await cot::tcp_connect("127.0.0.1:9000");


    while (true) {
        co_await cot::after(50s);
    }
}





int main(int argc, char *argv[]) {
    if (argc > 1)
        printf("Too many arguments supplied.\n");

    if (strcmp(basename(argv[0]), "server") == 0) {
        run_server().detach();
    } else if (strcmp(basename(argv[0]), "client") == 0) {
        run_client().detach();
    } else {
        fprintf(stderr, "Sorry, command %s not recognized\n", basename(argv[0]));
    }

    cot::loop();



    printf("Exiting\n");

    return 0;
}
