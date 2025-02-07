//g++ server.cpp rpc.pb.cc -o server -lprotobuf -pthread
#include <iostream>
#include <vector>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <map>
#include <functional>
#include "rpc.pb.h" // 假设你使用的是 ProtoBuf

using namespace std;

// 模拟 RPC 方法注册表
map<string, function<string(const string&)> > methods_ = {
    {"echo", [](const string& params) { return params; }},
    {"reverse", [](const string& params) { return string(params.rbegin(), params.rend()); }}
};

// 处理客户端请求
void HandleClient(int client_socket) {
    char buffer[1024] = {0};
    int valread = read(client_socket, buffer, 1024);

    if (valread <= 0) {
        if (valread == 0) {
            std::cout << "Client disconnected" << std::endl;
        } else {
            perror("Read failed");
        }
        close(client_socket);
        return;
    }

    rpc::RpcRequest request;
    if (!request.ParseFromString(string(buffer, valread))) {
        cerr << "Failed to parse request" << endl;
        close(client_socket);
        return;
    }

    rpc::RpcResponse response;
    auto it = methods_.find(request.method());
    if (it != methods_.end()) {
        try {
            string result = it->second(request.params());
            response.set_code(0);
            response.set_result(result);
        } catch (const exception& e) {
            response.set_code(1);
            response.set_error(e.what());
        }
    } else {
        response.set_code(1);
        response.set_error("Method not found");
    }

    string response_data;
    response.SerializeToString(&response_data);
    ssize_t bytes_sent = send(client_socket, response_data.c_str(), response_data.size(), 0);
    if (bytes_sent < 0) {
        perror("Send failed");
    }

    close(client_socket);
}

// 子进程逻辑：监听指定端口并处理客户端请求
void RunServer(int port) {
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
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    cout << "Server listening on port " << port << endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        // 处理客户端请求
        HandleClient(client_socket);
    }
}

// 主进程管理子进程
vector<pid_t> child_pids;

// 信号处理：终止所有子进程
void SignalHandler(int signum) {
    cout << "Terminating all child processes..." << endl;
    for (pid_t pid : child_pids) {
        kill(pid, SIGTERM);
    }
    while (wait(NULL) > 0); // 等待所有子进程退出
    exit(0);
}

int main() {
    signal(SIGINT, SignalHandler); // 捕获 Ctrl+C 信号

    vector<int> ports = {5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009};

    for (int port : ports) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程逻辑
            RunServer(port);
            exit(0); // 子进程退出
        } else if (pid > 0) {
            // 主进程记录子进程 PID
            child_pids.push_back(pid);
        } else {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        }
    }

    cout << "All servers started. Press Ctrl+C to stop." << endl;

    // 主进程等待子进程
    while (true) {
        pause(); // 等待信号
    }

    return 0;
}
