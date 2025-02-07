//g++ server.cpp rpc.pb.cc -o server -lprotobuf -pthread -lzookeeper_mt
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
#include <zookeeper/zookeeper.h> // ZooKeeper 客户端库
#include <ifaddrs.h>  // 用于获取本地 IP 地址
#include <netdb.h>    // 提供 NI_MAXHOST、NI_NUMERICHOST、getnameinfo 和 gai_strerror
#include <future>     // 用于异步任务
#include "rpc.pb.h"   // 假设你使用的是 ProtoBuf

using namespace std;

// ZooKeeper 全局变量
zhandle_t* zk_handle = nullptr;
const char* zk_servers = "192.168.248.111:2181,192.168.248.112:2181,192.168.248.113:2181";
const char* zk_root_path = "/servers";

// 主进程管理子进程
vector<pid_t> child_pids;

//端口号
int port=5000;

// 模拟 RPC 方法注册表
map<string, function<string(const string&)> > methods_ = {
    {"echo", [](const string& params) { return params; }},
    {"reverse", [](const string& params) { return string(params.rbegin(), params.rend()); }}
};

// 获取本地 IP 地址
string GetLocalIPAddress() {
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "127.0.0.1"; // 如果获取失败，返回默认地址
    }

    string local_ip = "127.0.0.1"; // 默认值

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        // 只处理 IPv4 地址
        if (ifa->ifa_addr->sa_family == AF_INET) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (s != 0) {
                cerr << "getnameinfo() failed: " << gai_strerror(s) << endl;
                continue;
            }

            // 排除 127.0.0.1
            if (strcmp(host, "127.0.0.1") != 0) {
                local_ip = host; // 找到第一个非 127.0.0.1 的地址
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return local_ip;
}

// ZooKeeper 异步回调函数：创建节点
void CreateNodeCallback(int rc, const char* value, const void* data) {
    if (rc == ZOK) {
        cout << "Node created successfully: " << (value ? value : "unknown") << endl;
    } else {
        cerr << "Failed to create node, error: " << zerror(rc) << endl;
    }
}

// ZooKeeper 异步回调函数：检查节点是否存在
void ExistsCallback(int rc, const struct Stat* stat, const void* data) {
    if (rc == ZNONODE) {
        // 根路径不存在，创建根路径
        int ret = zoo_acreate(zk_handle, zk_root_path, nullptr, 0, &ZOO_OPEN_ACL_UNSAFE, 0, CreateNodeCallback, nullptr);
        if (ret != ZOK) {
            cerr << "Failed to initiate creation of root path: " << zerror(ret) << endl;
        }
    } else if (rc == ZOK) {
        cout << "Root path exists in ZooKeeper." << endl;
    } else {
        cerr << "Failed to check root path, error: " << zerror(rc) << endl;
    }
}

// 注册服务节点到 ZooKeeper
void RegisterToZooKeeperAsync(const string& node_path, const string& node_data) {
    cout << "Registering service node: " << node_path << " with data: " << node_data << endl;

    int ret = zoo_acreate(zk_handle, node_path.c_str(), node_data.c_str(), node_data.size(),
                          &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL,
                          [](int rc, const char* value, const void* data) {
                              if (rc == ZOK) {
                                  cout << "Successfully registered service node: " << value << endl;
                              } else {
                                  cerr << "Failed to register service node, error: " << zerror(rc) << endl;
                              }
                          },
                          nullptr);
    if (ret != ZOK) {
        cerr << "zoo_acreate failed to send request, error: " << zerror(ret) << endl;
    }
}

// 处理客户端请求
void HandleClientAsync(int client_socket) {
    // 异步处理客户端请求
    auto task = std::async(std::launch::async, [client_socket]() {
        char buffer[1024] = {0};
        int valread = read(client_socket, buffer, sizeof(buffer));

        if (valread <= 0) {
            if (valread == 0) {
                std::cout << "Client disconnected" << std::endl;
            } else {
                perror("Read failed");
            }
            close(client_socket);
            return;
        }

        // 解析客户端请求
        rpc::RpcRequest request;
        if (!request.ParseFromString(string(buffer, valread))) {
            cerr << "Failed to parse request from client" << endl;
            close(client_socket);
            return;
        }

        // 打印请求信息
        cout << "Received RPC request: method=" << request.method() 
             << ", params=" << request.params() << endl;

        // 构造响应
        rpc::RpcResponse response;
        auto it = methods_.find(request.method());
        if (it != methods_.end()) {
            try {
                string result = it->second(request.params());
                response.set_code(0);
                response.set_result(result);
                cout << "Successfully executed method: " << request.method() 
                     << ", result=" << result << endl;
            } catch (const exception& e) {
                response.set_code(1);
                response.set_error(e.what());
                cerr << "Error executing method: " << request.method() 
                     << ", error=" << e.what() << endl;
            }
        } else {
            response.set_code(1);
            response.set_error("Method not found");
            cerr << "Method not found: " << request.method() << endl;
        }

        // 序列化并发送响应
        string response_data;
        if (!response.SerializeToString(&response_data)) {
            cerr << "Failed to serialize response" << endl;
            close(client_socket);
            return;
        }

        ssize_t bytes_sent = send(client_socket, response_data.c_str(), response_data.size(), 0);
        if (bytes_sent < 0) {
            perror("Send failed");
        } else {
            cout << "Response sent to client, bytes=" << bytes_sent << endl;
        }

        // 关闭客户端连接
        close(client_socket);
    });
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

    // 注册到 ZooKeeper
    string node_path = string(zk_root_path) + "/node_" + to_string(port);
    string node_data = GetLocalIPAddress() + ":" + to_string(port); // 假设服务运行在本地
    RegisterToZooKeeperAsync(node_path, node_data);                 //重点！！！为什么没有注册？

    while (true) {
        cout<<"进入while\n";
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        // 异步处理客户端请求
        HandleClientAsync(client_socket);
    }
}

// 处理 ZooKeeper 连接状态的 Watcher
void Watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {
    if (type == ZOO_SESSION_EVENT && state == ZOO_CONNECTED_STATE) {
        cout << "ZooKeeper connection established." << endl;
    } else if (state == ZOO_EXPIRED_SESSION_STATE) {
        cerr << "ZooKeeper session expired." << endl;
    }
}

// 确保根路径存在
void EnsureRootPathExists(zhandle_t* zk_handle=zk_handle,const char * zk_root_path=zk_root_path) {
    int ret = zoo_aexists(zk_handle, zk_root_path, 0,
                          [](int rc, const struct Stat* stat, const void* data) {
                              if (rc == ZNONODE) {
                                  cout << "Root path does not exist. Creating..." << endl;
                                  zoo_acreate(zk_handle, zk_root_path, nullptr, 0, &ZOO_OPEN_ACL_UNSAFE, 0,
                                              [](int rc, const char* value, const void* data) {
                                                  if (rc == ZOK) {
                                                      cout << "Root path created successfully: " << zk_root_path << endl;
                                                  } else {
                                                      cerr << "Failed to create root path, error: " << zerror(rc) << endl;
                                                  }
                                              },
                                              nullptr);
                              } else if (rc == ZOK) {
                                  cout << "Root path exists." << endl;
                              } else {
                                  cerr << "Failed to check root path, error: " << zerror(rc) << endl;
                              }
                          },
                          nullptr);
    if (ret != ZOK) {
        cerr << "zoo_aexists failed, error: " << zerror(ret) << endl;
    }
}

int main() {

    // 初始化 ZooKeeper 连接
    zk_handle = zookeeper_init(zk_servers, Watcher, 30000, 0, nullptr, 0);
    if (!zk_handle) {
        cerr << "Failed to connect to ZooKeeper" << endl;
        return EXIT_FAILURE;
    }

    //检查根目录是否存在
    EnsureRootPathExists();

    //
    RunServer(port);

    return 0;
}