//g++ server.cpp rpc.pb.cc -o server -lprotobuf -pthread
#include <iostream>
#include <unordered_map>
#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "rpc.pb.h"

using namespace std;

class RpcServer {
public:
    RpcServer(int port) : port_(port) {}

    void RegisterMethod(const string& name, function<string(const string&)> func) {
        methods_[name] = func;
    }

    void Start() {
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

        cout << "RPC Server listening on port " << port_ << endl;
        
        socklen_t addrlen = sizeof(address);

        while (true) {
            int client_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
            if (client_socket < 0) {
                perror("Accept failed");
                continue;
            }

            thread(&RpcServer::HandleClient, this, client_socket).detach();
        }
    }

private:
    int port_;
    unordered_map<string, function<string(const string&)>> methods_;

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
};

string Add(const string& params) {
    size_t comma_pos = params.find(',');
    if (comma_pos == string::npos) throw invalid_argument("Invalid params format");

    int a = stoi(params.substr(0, comma_pos));
    int b = stoi(params.substr(comma_pos + 1));
    return to_string(a + b);
}

int main() {
    RpcServer server(5000);
    server.RegisterMethod("Add", Add);
    server.Start();
    return 0;
}