#include "bittorrent.hh"
#include "cotamer/http.hh"
#include "bencode.h"
#include "cotamer/cotamer.hh"
#include "tracker/http.hh"
#include "tracker/state.hh"
#include <libgen.h>
#include <cstdio>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#define MAXLEN 256;

namespace cot = cotamer;
using namespace std::chrono_literals;

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
