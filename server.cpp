// server.cpp
#include "server.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <thread>
#include <filesystem>
#include <cstring>
#include <signal.h>
#include <fstream>
#include <system_error>
#include <iomanip>
#include <ctime>
#include <algorithm>

FTPServer::FTPServer(int port, const std::string& root_dir) 
    : port_(port), 
      root_directory_(root_dir),
      current_directory_(root_dir),
      running_(false),
      server_socket_(-1),
      data_port_(port + 1),
      passive_socket_(-1),
      client_socket_(-1) {
    initializeCommands();
    signal(SIGPIPE, SIG_IGN);
}

FTPServer::~FTPServer() {
    stop();
}

void FTPServer::start() {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int reuse = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        throw std::runtime_error("Bind failed");
    }

    if (listen(server_socket_, 5) < 0) {
        throw std::runtime_error("Listen failed");
    }

    running_ = true;
    std::cout << "FTP Server started on port " << port_ << std::endl;

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int new_client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (new_client_socket < 0) {
            if (!running_) break;
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        std::string welcome_msg = "220 Welcome to Simple FTP Server\r\n";
        send(new_client_socket, welcome_msg.c_str(), welcome_msg.length(), 0);

        std::thread client_thread(&FTPServer::handleClient, this, new_client_socket);
        client_thread.detach();
    }
}

void FTPServer::handleClient(int client_socket) {
    client_socket_ = client_socket;
    char buffer[1024];
    while (running_) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            break;
        }

        std::string command_line(buffer);
        std::string command, argument;
        
        size_t space_pos = command_line.find(' ');
        if (space_pos != std::string::npos) {
            command = command_line.substr(0, space_pos);
            argument = command_line.substr(space_pos + 1);
            argument = argument.substr(0, argument.find("\r\n"));
        } else {
            command = command_line.substr(0, command_line.find("\r\n"));
        }

        std::transform(command.begin(), command.end(), command.begin(), ::toupper);

        auto it = commands_.find(command);
        std::string response;
        if (it != commands_.end()) {
            response = it->second(argument);
        } else {
            response = "500 Unknown command\r\n";
        }

        if (send(client_socket, response.c_str(), response.length(), 0) < 0) {
            break;
        }
        
        if (command == "QUIT") {
            break;
        }
    }

    close(client_socket);
    client_socket_ = -1;
}

void FTPServer::initializeCommands() {
    commands_["USER"] = [](const std::string& arg) -> std::string {
        return "230 User logged in\r\n";
    };

    commands_["SYST"] = [](const std::string& arg) -> std::string {
        return "215 UNIX Type: L8\r\n";
    };

    commands_["FEAT"] = [](const std::string& arg) -> std::string {
        return "211-Features:\r\n PASV\r\n UTF8\r\n211 End\r\n";
    };

    commands_["TYPE"] = [](const std::string& arg) -> std::string {
        return "200 Type set to I\r\n";
    };

    commands_["PWD"] = [this](const std::string& arg) -> std::string {
        std::filesystem::path rel_path = std::filesystem::relative(current_directory_, root_directory_);
        return "257 \"/" + rel_path.string() + "\"\r\n";
    };

    commands_["CWD"] = [this](const std::string& arg) -> std::string {
        std::string new_path = resolvePath(arg);
        if (validatePath(new_path) && std::filesystem::exists(new_path)) {
            current_directory_ = new_path;
            return "250 Directory successfully changed.\r\n";
        }
        return "550 Failed to change directory.\r\n";
    };

    commands_["PASV"] = [this](const std::string& arg) -> std::string {
        if (passive_socket_ != -1) {
            close(passive_socket_);
            passive_socket_ = -1;
        }

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        getsockname(server_socket_, (struct sockaddr*)&addr, &addr_len);
        
        int data_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (data_sock < 0) {
            return "425 Can't open data connection.\r\n";
        }

        struct sockaddr_in data_addr;
        memset(&data_addr, 0, sizeof(data_addr));
        data_addr.sin_family = AF_INET;
        data_addr.sin_addr.s_addr = INADDR_ANY;
        data_addr.sin_port = 0;

        if (bind(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
            close(data_sock);
            return "425 Can't open data connection.\r\n";
        }

        if (listen(data_sock, 1) < 0) {
            close(data_sock);
            return "425 Can't open data connection.\r\n";
        }

        getsockname(data_sock, (struct sockaddr*)&data_addr, &addr_len);
        
        char host_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), host_ip, INET_ADDRSTRLEN);

        int port = ntohs(data_addr.sin_port);
        int p1 = port / 256;
        int p2 = port % 256;

        std::string ip_str(host_ip);
        std::replace(ip_str.begin(), ip_str.end(), '.', ',');

        passive_socket_ = data_sock;
        return "227 Entering Passive Mode (" + ip_str + "," + 
               std::to_string(p1) + "," + std::to_string(p2) + ")\r\n";
    };

    commands_["LIST"] = [this](const std::string& arg) -> std::string {
        if (passive_socket_ < 0) {
            return "425 Use PASV first.\r\n";
        }

        send(client_socket_, "150 Here comes the directory listing.\r\n", 38, 0);

        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int data_sock = accept(passive_socket_, (struct sockaddr*)&client_addr, &len);
        close(passive_socket_);
        passive_socket_ = -1;

        if (data_sock < 0) {
            return "425 Can't open data connection.\r\n";
        }

        try {
            std::stringstream listing;
            for (const auto& entry : std::filesystem::directory_iterator(current_directory_)) {
                auto last_write_time = std::filesystem::last_write_time(entry.path());
                auto file_size = entry.is_directory() ? 0 : std::filesystem::file_size(entry.path());
                
                listing << (entry.is_directory() ? "d" : "-")
                        << "rwxr-xr-x 1 ftp ftp "
                        << std::setw(12) << file_size << " "
                        << entry.path().filename().string() << "\r\n";
            }
            
            std::string list_str = listing.str();
            if (send(data_sock, list_str.c_str(), list_str.length(), 0) < 0) {
                close(data_sock);
                return "550 Failed to send directory listing.\r\n";
            }
            close(data_sock);
            
            return "226 Directory send OK.\r\n";
        } catch (const std::filesystem::filesystem_error& e) {
            close(data_sock);
            return "550 Failed to list directory: " + std::string(e.what()) + "\r\n";
        }
    };

    commands_["QUIT"] = [this](const std::string& arg) -> std::string {
        return "221 Goodbye.\r\n";
    };

    commands_["STOR"] = [this](const std::string& arg) -> std::string {
        if (passive_socket_ < 0) {
            return "425 Use PASV first.\r\n";
        }

        std::string filepath = resolvePath(arg);
        if (!validatePath(filepath)) {
            return "553 Invalid file path.\r\n";
        }

        send(client_socket_, "150 Opening data connection for file upload.\r\n", 46, 0);

        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int data_sock = accept(passive_socket_, (struct sockaddr*)&client_addr, &len);
        close(passive_socket_);
        passive_socket_ = -1;

        if (data_sock < 0) {
            return "425 Can't open data connection.\r\n";
        }

        try {
            std::ofstream file(filepath, std::ios::binary);
            if (!file) {
                close(data_sock);
                return "550 Failed to create file.\r\n";
            }

            char buffer[8192];
            ssize_t bytes_received;
            
            while ((bytes_received = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
                file.write(buffer, bytes_received);
            }

            file.close();
            close(data_sock);

            if (bytes_received < 0) {
                std::filesystem::remove(filepath);
                return "550 Failed to receive file.\r\n";
            }

            logActivity("File uploaded: " + arg);
            return "226 Transfer complete.\r\n";
        }
        catch (const std::exception& e) {
            close(data_sock);
            return "550 Upload failed: " + std::string(e.what()) + "\r\n";
        }
    };

    commands_["RETR"] = [this](const std::string& arg) -> std::string {
        if (passive_socket_ < 0) {
            return "425 Use PASV first.\r\n";
        }

        std::string filepath = resolvePath(arg);
        if (!validatePath(filepath) || !std::filesystem::exists(filepath) || 
            std::filesystem::is_directory(filepath)) {
            return "550 File not found or access denied.\r\n";
        }

        send(client_socket_, "150 Opening data connection for file download.\r\n", 48, 0);

        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int data_sock = accept(passive_socket_, (struct sockaddr*)&client_addr, &len);
        close(passive_socket_);
        passive_socket_ = -1;

        if (data_sock < 0) {
            return "425 Can't open data connection.\r\n";
        }

        try {
            std::ifstream file(filepath, std::ios::binary);
            if (!file) {
                close(data_sock);
                return "550 Failed to open file.\r\n";
            }

            char buffer[8192];
            while (!file.eof()) {
                file.read(buffer, sizeof(buffer));
                std::streamsize bytes_read = file.gcount();
                
                if (bytes_read > 0) {
                    ssize_t bytes_sent = send(data_sock, buffer, bytes_read, 0);
                    if (bytes_sent < 0) {
                        file.close();
                        close(data_sock);
                        return "550 Failed to send file.\r\n";
                    }
                }
            }

            file.close();
            close(data_sock);

            logActivity("File downloaded: " + arg);
            return "226 Transfer complete.\r\n";
        }
        catch (const std::exception& e) {
            close(data_sock);
            return "550 Download failed: " + std::string(e.what()) + "\r\n";
        }
    };

    commands_["SIZE"] = [this](const std::string& arg) -> std::string {
        std::string filepath = resolvePath(arg);
        if (!validatePath(filepath) || !std::filesystem::exists(filepath) || 
            std::filesystem::is_directory(filepath)) {
            return "550 File not found or access denied.\r\n";
        }

        try {
            auto size = std::filesystem::file_size(filepath);
            return "213 " + std::to_string(size) + "\r\n";
        }
        catch (const std::filesystem::filesystem_error&) {
            return "550 Could not get file size.\r\n";
        }
    };
// در تابع initializeCommands اضافه کنید:

commands_["DELE"] = [this](const std::string& arg) -> std::string {
    std::string filepath = resolvePath(arg);
    if (!validatePath(filepath) || !std::filesystem::exists(filepath)) {
        return "550 File not found.\r\n";
    }
    
    try {
        if (std::filesystem::remove(filepath)) {
            logActivity("File deleted: " + arg);
            return "250 File successfully deleted.\r\n";
        } else {
            return "550 Failed to delete file.\r\n";
        }
    } catch (const std::filesystem::filesystem_error& e) {
        return "550 Delete operation failed: " + std::string(e.what()) + "\r\n";
    }
};

commands_["NLST"] = [this](const std::string& arg) -> std::string {
    if (passive_socket_ < 0) {
        return "425 Use PASV first.\r\n";
    }

    send(client_socket_, "150 Here comes the file list.\r\n", 31, 0);

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int data_sock = accept(passive_socket_, (struct sockaddr*)&client_addr, &len);
    close(passive_socket_);
    passive_socket_ = -1;

    if (data_sock < 0) {
        return "425 Can't open data connection.\r\n";
    }

    try {
        std::stringstream listing;
        for (const auto& entry : std::filesystem::directory_iterator(current_directory_)) {
            listing << entry.path().filename().string() << "\r\n";
        }
        
        std::string list_str = listing.str();
        if (send(data_sock, list_str.c_str(), list_str.length(), 0) < 0) {
            close(data_sock);
            return "550 Failed to send file list.\r\n";
        }
        close(data_sock);
        
        return "226 Transfer complete.\r\n";
    } catch (const std::filesystem::filesystem_error& e) {
        close(data_sock);
        return "550 Failed to list files: " + std::string(e.what()) + "\r\n";
    }
};
}

void FTPServer::stop() {
    running_ = false;
    if (server_socket_ != -1) {
        close(server_socket_);
        server_socket_ = -1;
    }
    if (passive_socket_ != -1) {
        close(passive_socket_);
        passive_socket_ = -1;
    }
}

std::string FTPServer::resolvePath(const std::string& path) {
    if (path.empty()) return current_directory_;
    
    std::filesystem::path full_path;
    if (path[0] == '/') {
        // استفاده از std::filesystem::path برای ترکیب مسیرها
        full_path = std::filesystem::path(root_directory_) / path.substr(1);
    } else {
        full_path = std::filesystem::path(current_directory_) / path;
    }
    
    return std::filesystem::absolute(full_path).string();
}
bool FTPServer::validatePath(const std::string& path) const {
    try {
        std::filesystem::path full_path = std::filesystem::absolute(path);
        std::filesystem::path root = std::filesystem::absolute(root_directory_);
        auto rel = std::filesystem::relative(full_path, root);
        return !rel.string().empty() && rel.string().substr(0, 2) != "..";
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

void FTPServer::logActivity(const std::string& message) {
    std::time_t now = std::time(nullptr);
    std::cout << "[" << std::put_time(std::localtime(&now), "%F %T") << "] " << message << std::endl;
}
