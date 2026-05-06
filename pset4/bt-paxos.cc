#include <algorithm>
#include "cotamer/http.hh"
#include "cotamer/cotamer.hh"
#include <libgen.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <print>
#include <random>

#define MAXLEN 256;

namespace cot = cotamer;
using namespace std::chrono_literals;

struct message {
    char message[256];
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

        const char* url = req.url().c_str();
        printf("%s url reqd\n", url);

        if (strcmp(url, "/success") == 0) {
            res.status_code(200)
                .header("Content-Type", "text/plain")
                .body(std::format("Success!"));
                co_await hp.send(std::move(res));
        } else {
            res.status_code(404)
                .header("Content-Type", "text/plain")
                .body(std::format("you asked for {}\n", req.url()));
                co_await hp.send(std::move(res));
        }
        
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
