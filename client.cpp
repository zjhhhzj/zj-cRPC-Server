//g++ client.cpp rpc.pb.cc -o client -lprotobuf
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "rpc.pb.h"

using namespace std;

class RpcClient {
public:
    RpcClient(const string& server_ip, int port) : server_ip_(server_ip), port_(port) {}

    string CallMethod(const string& method, const string& params) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, server_ip_.c_str(), &serv_addr.sin_addr) <= 0) {
            perror("Invalid address or address not supported");
            exit(EXIT_FAILURE);
        }

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("Connection failed");
            exit(EXIT_FAILURE);
        }

        // 构造请求
        rpc::RpcRequest request;
        request.set_method(method);
        request.set_params(params);

        string request_data;
        request.SerializeToString(&request_data);

        send(sock, request_data.c_str(), request_data.size(), 0);

        char buffer[1024] = {0};
        int valread = read(sock, buffer, 1024);

        // 解析响应
        rpc::RpcResponse response;
        response.ParseFromString(string(buffer, valread));

        close(sock);

        if (response.code() == 0) {
            return response.result();
        } else {
            throw runtime_error(response.error());
        }
    }

private:
    string server_ip_;
    int port_;
};

int main() {
    RpcClient client("127.0.0.1", 5000);

    try {
        string result = client.CallMethod("Add", "10,20");
        cout << "Result: " << result << endl;
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
    }

    return 0;
}
/*
cerr：

是 C++ 标准库中的标准错误流对象，定义在 <iostream> 头文件中。

用于输出错误信息，通常与 cout（标准输出流）区分开，便于区分正常输出和错误输出。

e.what()：

e 是一个异常对象，通常是 std::exception 或其派生类的实例。

what() 是 std::exception 的成员函数，返回一个描述异常信息的 C 风格字符串（const char*）。
*/