#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "hello.pb.h"

#pragma comment(lib, "Ws2_32.lib") // 链接 Windows 套接字库

#define PORT 8080

void handleClient(SOCKET clientSocket) {
    char buffer[1024] = {0};

    // 接收客户端请求
    int bytesRead = recv(clientSocket, buffer, 1024, 0);
    if (bytesRead > 0) {
        // 解析请求消息
        helloworld::HelloRequest request;
        if (request.ParseFromArray(buffer, bytesRead)) {
            std::cout << "Received request: " << request.name() << std::endl;

            // 构造响应消息
            helloworld::HelloReply reply;
            reply.set_message("Hello, " + request.name());

            // 序列化响应消息并发送
            std::string replyData;
            reply.SerializeToString(&replyData);
            send(clientSocket, replyData.c_str(), replyData.size(), 0);
        } else {
            std::cerr << "Failed to parse request." << std::endl;
        }
    }

    closesocket(clientSocket);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return -1;
    }

    SOCKET serverSocket, clientSocket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // 创建套接字
    if ((serverSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        WSACleanup();
        return -1;
    }

    // 配置地址和端口
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 绑定套接字
    if (bind(serverSocket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return -1;
    }

    // 监听连接
    if (listen(serverSocket, 3) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closes
