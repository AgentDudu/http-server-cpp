#include "app/signals.h"
#include "net/context.h"
#include "server/server.h"
#include "server/static_file.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (port < 1 || port > 65535) {
        std::fprintf(stderr, "usage: http_server [port] [root]\n");
        return 2;
    }

    try {
        net::Context ctx;
        app::ShutdownSignals signals;
        const std::string root = argc > 2 ? argv[2] : ".";
        server::StaticFiles files(root);
        server::Server server(static_cast<std::uint16_t>(port), files);
        std::printf("serving %s on http://127.0.0.1:%d/ (stop with Ctrl-C)\n",
                    files.root().string().c_str(), port);
        std::fflush(stdout);
        server.run(signals);
        std::puts("stopped");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
