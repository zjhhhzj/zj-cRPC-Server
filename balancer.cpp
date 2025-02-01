//g++ balancer.cpp  -o balancer
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

class LoadBalancer {
public:
    LoadBalancer(int port, const vector<int>& server_ports)
        : port_(port), server_ports_(server_ports), connection_count_(0) {}

    void Start() {
        // 提前与后端服务器建立连接
        if (!InitializeServerConnections()) {
            cerr << "Failed to initialize server connections" << endl;
            exit(EXIT_FAILURE);
        }

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("Set socket options failed");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }

        if (listen(server_fd, SOMAXCONN) < 0) {
            perror("Listen failed");
            exit(EXIT_FAILURE);
        }

        cout << "Load Balancer listening on port " << port_ << endl;

        socklen_t addrlen = sizeof(address);

        while (true) {
            int client_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
            if (client_socket < 0) {
                perror("Accept failed");
                continue;
            }

            // 创建新线程处理转发逻辑
            thread(&LoadBalancer::ForwardRequest, this, client_socket).detach();
        }
    }

private:
    int port_;
    vector<int> server_ports_;
    vector<int> server_sockets_; // 保存与后端服务器的长连接
    int connection_count_;
    mutex count_mutex_;

    // 初始化与后端服务器的长连接
    bool InitializeServerConnections() {
        for (int target_port : server_ports_) {
            int server_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (server_socket < 0) {
                perror("Socket creation failed");
                return false;
            }

            struct sockaddr_in server_addr;
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(target_port);
            if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
                perror("Invalid address");
                close(server_socket);
                return false;
            }

            if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                perror("Connection to server failed");
                close(server_socket);
                return false;
            }

            // 保存连接
            server_sockets_.push_back(server_socket);
            cout << "Connected to server on port " << target_port << endl;
        }
        return true;
    }

    void ForwardRequest(int client_socket) {
        // 确定目标服务器
        int server_index;
        {
            lock_guard<mutex> lock(count_mutex_);
            server_index = connection_count_ % server_ports_.size();
            connection_count_++;
        }

        int server_socket = server_sockets_[server_index];
        cout << "Forwarding request to server on port " << server_ports_[server_index] << endl;

        // 转发数据：从客户端读取，发送到目标服务器
        char buffer[1024];
        int valread = read(client_socket, buffer, 1024);
        if (valread > 0) {
            // 尝试将数据发送到目标服务器
            ssize_t bytes_sent = send(server_socket, buffer, valread, 0);
            if (bytes_sent < 0) {
                perror("Send to server failed");

                // 如果发送失败，说明连接可能已失效，需要移除该连接
                RemoveServerConnection(server_index);
                close(client_socket); // 关闭客户端连接
                return;
            }

            // 尝试从目标服务器读取响应
            int valwrite = read(server_socket, buffer, 1024);
            if (valwrite > 0) {
                send(client_socket, buffer, valwrite, 0);
            } else if (valwrite <= 0) {
                // 如果读取失败，说明服务器连接可能已关闭
                perror("Read from server failed");
                RemoveServerConnection(server_index);
            }
        }

        close(client_socket); // 关闭客户端连接，但保持与后端服务器的长连接
    }

    void RemoveServerConnection(int server_index) {
        lock_guard<mutex> lock(count_mutex_);

        // 关闭失效的 socket
        int failed_socket = server_sockets_[server_index];
        close(failed_socket);

        // 从 server_sockets_ 和 server_ports_ 中移除失效的服务器
        server_sockets_.erase(server_sockets_.begin() + server_index);
        server_ports_.erase(server_ports_.begin() + server_index);

        cout << "Removed server connection on port " << server_ports_[server_index] << endl;
    }
};

int main() {
    vector<int> server_ports = {5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009, 5010};
    LoadBalancer lb(5555, server_ports);
    lb.Start();
    return 0;
}
