#include "app/signals.h"
#include "http/request.h"
#include "net/context.h"
#include "server/server.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (port < 1 || port > 65535) {
        std::fprintf(stderr, "usage: http_server [port]\n");
        return 2;
    }

    try {
        net::Context ctx;
        app::ShutdownSignals signals;
        server::Server server(static_cast<std::uint16_t>(port),
                              [](const http::Request&) {
                                  return std::string("Hello from http-server-cpp\n");
                              });
        std::printf("listening on http://127.0.0.1:%d/ (stop with Ctrl-C)\n", port);
        std::fflush(stdout);
        server.run(signals);
        std::puts("stopped");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
