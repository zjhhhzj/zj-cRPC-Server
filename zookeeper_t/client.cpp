//g++ client.cpp rpc.pb.cc -o client -lprotobuf -lzookeeper_mt
#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <zookeeper/zookeeper.h>
#include "rpc.pb.h"

using namespace std;

// 全局变量，用于存储负载均衡节点
vector<string> load_balancer_nodes;

// Zookeeper 回调函数
void watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {}

// 异步获取负载均衡节点的回调函数
void GetChildrenCallback(int rc, const struct String_vector* strings, const void* data) {
    if (rc != ZOK) {
        cerr << "Error getting children: " << zerror(rc) << endl;
        return;
    }

    load_balancer_nodes.clear();
    for (int i = 0; i < strings->count; ++i) {
        load_balancer_nodes.emplace_back(strings->data[i]);
    }

    cout << "Load balancer nodes updated successfully." << endl;
}

// 获取负载均衡节点（异步版本）
void getLoadBalancerNodes(zhandle_t* zh, const string& path) {
    int ret = zoo_aget_children(zh, path.c_str(), 0, GetChildrenCallback, nullptr);
    if (ret != ZOK) {
        cerr << "Error initiating async get children: " << zerror(ret) << endl;
    }
}

// RPC 客户端类
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
    // Zookeeper 集群地址
    string zookeeper_hosts = "192.168.248.111:2181,192.168.248.112:2181,192.168.248.113:2181";

    // 连接到 Zookeeper
    zhandle_t* zh = zookeeper_init(zookeeper_hosts.c_str(), watcher, 30000, nullptr, nullptr, 0);
    if (!zh) {
        cerr << "Failed to connect to Zookeeper" << endl;
        return -1;
    }

    // 获取负载均衡节点
    string path = "/balancers";
    getLoadBalancerNodes(zh, path);

    // 等待异步回调完成
    sleep(1); // 简单的等待方式，实际应用中可以使用更优雅的同步机制

    if (load_balancer_nodes.empty()) {
        cerr << "No available load balancer nodes!" << endl;
        zookeeper_close(zh);
        return -1;
    }

    // 选择第一个负载均衡节点
    string selected_node = load_balancer_nodes[0];
    size_t pos = selected_node.find(':');
    string server_ip = selected_node.substr(0, pos);
    int port = stoi(selected_node.substr(pos + 1));

    cout << "Connecting to load balancer: " << server_ip << ":" << port << endl;

    // 创建 RPC 客户端并调用方法
    RpcClient client(server_ip, port);

    try {
        string result = client.CallMethod("reverse", "zjhhh");
        cout << "Result: " << result << endl;
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
    }

    // 关闭 Zookeeper 连接
    zookeeper_close(zh);

    return 0;
}
