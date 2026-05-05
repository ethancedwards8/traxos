#include "pt-paxos.hh"
#include <algorithm>
#include <libgen.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <print>
#include <ptrcheck.h>
#include <random>

#define MAXLEN 256;

namespace cot = cotamer;
using namespace std::chrono_literals;

struct message {
    char message[256];
};

// cot::task<bool> send(struct message message) {

// }

char buf[4096];

cot::task<> run_server() {
    printf("Running the server\n");
    cot::fd lfd = co_await cot::tcp_listen("127.0.0.1:9000");
    while (true) {
        auto cfd = co_await cot::tcp_accept(lfd);
        printf("accepted\n");

        auto n = co_await cot::read_once(lfd, buf, sizeof(buf));
        printf("read\n\r\n\r\n");
        if (n == 0) {
            break;
        }

    }
}


cot::task<> run_client() {
    cot::fd lfd = co_await cot::tcp_connect("127.0.0.1:9000");

    while (true) {
        co_await cot::after(50s);
        printf("Running as the client\n\r\n\r\n");
        co_await cot::write(lfd, "echo hello hi", 14);
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



    printf("Buffer: %s\n", buf);

    printf("Exiting\n");

    return 0;
}
