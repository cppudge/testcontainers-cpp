#include <iostream>

#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Connects to the local Docker daemon over its native transport (Windows named
// pipe / unix socket / TCP) and issues `GET /_ping`. This is the first real
// round-trip to the Docker Engine API.
int main() {
    using namespace testcontainers;
    try {
        const DockerHost host = DockerHost::resolve();
        std::cout << "Docker host: " << host.to_string() << '\n';

        DockerClient client{host};
        const Response res = client.request("GET", "/_ping");

        std::cout << "GET /_ping -> " << res.status_code << ' ' << res.reason << " (body: \""
                  << res.body << "\")\n";
        std::cout << "  Api-Version:  " << res.header("Api-Version") << '\n';
        std::cout << "  OSType:       " << res.header("OSType") << '\n';
        std::cout << "  Builder:      " << res.header("Builder-Version") << '\n';

        if (!res.ok()) {
            std::cerr << "Daemon did not return a 2xx status\n";
            return 1;
        }
        std::cout << "OK: Docker daemon reachable.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
