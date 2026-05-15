#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

#include "Services/ServiceGateway.h"

using namespace servicegateway;

// Global service gateway for signal handling
static ServiceGateway *g_serviceGateway = nullptr;

// Signal handler for graceful shutdown
void signalHandler(const int signal)
{
    std::cout << "\nReceived signal " << signal << ", shutting down ServiceGateway..." << '\n';

    if (g_serviceGateway != nullptr)
    {
        g_serviceGateway->stop();
    }

    // Restore default signal handler and re-raise signal
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

int main(const int argc, char *argv[])
{
    std::cout << "=== ServiceGateway v2.0 ===\n";

    try
    {
        // Parse command line arguments
        int port = 8080;
        std::string unixSocket = "/tmp/service_gateway.sock";

        if (argc >= 2)
        {
            port = std::stoi(argv[1]);
        }
        if (argc >= 3)
        {
            unixSocket = argv[2];
        }

        std::cout << "Starting ServiceGateway:" << '\n';
        std::cout << "  TCP Port: " << port << '\n';
        std::cout << "  UNIX Socket: " << unixSocket << '\n';

        // Create and start ServiceGateway
        ServiceGateway gateway(port, unixSocket);
        g_serviceGateway = &gateway;

        // Register signal handlers for graceful shutdown
        std::signal(SIGINT, signalHandler);  // Ctrl+C
        std::signal(SIGTERM, signalHandler); // kill command

        // Start the broker
        gateway.start();

        std::cout << "\nServiceGateway started successfully!" << '\n';
        std::cout << "Services can connect to:" << '\n';
        std::cout << "  TCP: tcp://localhost:" << port << '\n';
        std::cout << "  UNIX: unix://" << unixSocket << '\n';
        std::cout << "\nPress Ctrl+C to stop." << '\n';

        // Keep running until interrupted
        while (gateway.isRunning())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error starting ServiceGateway: " << e.what() << '\n';
        return 1;
    }

    std::cout << "ServiceGateway shutdown complete." << '\n';
    return 0;
}
