#include "HttpGateway.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

using namespace servicegateway;

static ServiceGateway *g_serviceGateway = nullptr;
static HttpGateway *g_httpGateway = nullptr;

void signalHandler(const int signal)
{
    std::cout << "\nReceived signal " << signal << ", shutting down gateways..." << '\n';

    if (g_httpGateway != nullptr) {
        g_httpGateway->stop();
    }

    if (g_serviceGateway != nullptr) {
        g_serviceGateway->stop();
    }

    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

int main(const int argc, char *argv[])
{
    int brokerPort = 8080;
    int httpPort = 3001;
    std::string unixSocket = "/tmp/service_gateway.sock";

    if (argc >= 2) {
        brokerPort = std::stoi(argv[1]);
    }
    if (argc >= 3) {
        httpPort = std::stoi(argv[2]);
    }
    if (argc >= 4) {
        unixSocket = argv[3];
    }

    std::cout << "=== ServiceGateway HTTP Bridge ===\n";
    std::cout << "Broker port: " << brokerPort << '\n';
    std::cout << "HTTP port:   " << httpPort << '\n';
    std::cout << "UNIX socket: " << unixSocket << '\n';

    try {
        ServiceGateway gateway(brokerPort, unixSocket);
        HttpGateway httpGateway(gateway, httpPort);

        g_serviceGateway = &gateway;
        g_httpGateway = &httpGateway;

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        if (!gateway.start()) {
            std::cerr << "Failed to start ServiceGateway" << '\n';
            return 1;
        }

        if (!httpGateway.start()) {
            std::cerr << "Failed to start HTTP gateway" << '\n';
            gateway.stop();
            return 1;
        }

        std::cout << "\nGateway ready:" << '\n';
        std::cout << "  internal tcp://localhost:" << brokerPort << '\n';
        std::cout << "  internal unix://" << unixSocket << '\n';
        std::cout << "  http://localhost:" << httpPort << " /invoke/{capability}" << '\n';
        std::cout << "  http://localhost:" << httpPort << " /status" << '\n';
        std::cout << "  http://localhost:" << httpPort << " /connections" << '\n';

        while (gateway.isRunning() || httpGateway.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception &exception) {
        std::cerr << "Error starting HTTP gateway: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}