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
#include <string_view>

#define MAXLEN 256;

namespace cot = cotamer;
using namespace std::chrono_literals;

// https://stackoverflow.com/questions/9443957/using-sizeof-on-arrays-passed-as-parameters
template <typename T>
T parse_number(std::string_view value) {
    T result{};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    assert(ec == std::errc() && ptr == value.data() + value.size());
    return result;
}

template <size_t N>
void copy_exact(char (&field)[N], std::string_view value) {
    assert(value.size() == N);
    memcpy(field, value.data(), N);
}

uint32_t parse_ipv4(std::string_view value) {
    uint32_t parts[4];
    size_t start = 0;

    for (int i = 0; i != 4; ++i) {
        size_t dot = value.find('.', start);
        if (i != 3) {
            assert(dot != std::string_view::npos);
        } else {
            assert(dot == std::string_view::npos);
            dot = value.size();
        }

        uint32_t part = parse_number<uint32_t>(value.substr(start, dot - start));
        assert(part <= 255);
        parts[i] = part;
        start = dot + 1;
    }

    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

struct message {
    char message[256];
};

struct TrackerResponse {

};

// cot::task<bool> send(struct message message) {

// }

cot::task<> run_server() {
    printf("Running the server\n");
    cot::fd lfd = co_await cot::tcp_listen("127.0.0.1:9000");
    cot::fd cfd = co_await cot::tcp_accept(lfd);

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

        if (url == "/announce") {


            for (auto it = req.search_param_begin(); it != req.search_param_end(); it++) {
                std::string_view name(it.name());
                // live laugh love c++
                std::string value(it.value());

                std::cout << name << " and " << value << std::endl;

                if (name == "info_hash") {

                    copy_exact(bt_req.info_hash, value);

                } else if (name == "peer_id") {

                    copy_exact(bt_req.peer_id, value);

                } else if (name == "ip") {

                    bt_req.ip = parse_ipv4(value);

                } else if (name == "port") {
                    
                    bt_req.port = parse_number<uint16_t>(value);

                } else if (name == "uploaded") {

                    bt_req.uploaded = parse_number<uint64_t>(value);

                } else if (name == "downloaded") {

                    bt_req.downloaded = parse_number<uint64_t>(value);

                } else if (name == "left") {

                    bt_req.left = parse_number<uint64_t>(value);

                } else if (name == "event") {

                    if (value == "started") {

                        bt_req.event = started;

                    } else if (value == "stopped") {

                        bt_req.event = stopped;

                    } else if (value == "completed") {

                        bt_req.event = completed;

                    } else {

                        assert(false);

                    }

                } else if (name == "numwant") {

                    bt_req.numwant = parse_number<int32_t>(value);

                } else {

                }
            }


            res.status_code(200)
                .header("Content-Type", "text/plain")
                .body();
        } else {
            res.status_code(404)
                .header("Content-Type", "text/plain")
                .body(std::format("you asked for {}\n", req.url()));
        }
        
        co_await hp.send(std::move(res));
    } while (hp.should_keep_alive());
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
