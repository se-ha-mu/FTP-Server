// server.h
#pragma once

#include <string>
#include <map>
#include <functional>

class FTPServer {
public:
    FTPServer(int port, const std::string& root_dir);
    ~FTPServer();

    void start();
    void stop();

private:
    using FTPCommand = std::function<std::string(const std::string&)>;
    int passive_socket_ = -1;     

    void initializeCommands();
    void handleClient(int client_socket);
    int setupDataConnection();
    std::string resolvePath(const std::string& path);
    bool validatePath(const std::string& path) const;
    void logActivity(const std::string& message);

    int port_;
    int data_port_;
    std::string root_directory_;
    std::string current_directory_;
    bool running_;
    int server_socket_;
    int client_socket_;
    std::map<std::string, std::function<std::string(const std::string&)>> commands_;
};
