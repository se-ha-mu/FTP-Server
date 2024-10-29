#include "server.h"
#include <iostream>
#include <filesystem>

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <port> <root_directory>" << std::endl;
            return 1;
        }

        int port = std::stoi(argv[1]);
        std::string root_dir = argv[2];

        // Create root directory if it doesn't exist
        std::filesystem::create_directories(root_dir);

        FTPServer server(port, root_dir);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
