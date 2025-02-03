// g++ balancer.cpp -o balancer -lzookeeper_mt -pthread
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>  // 用于获取本地 IP 地址
#include <netdb.h>    // 用于 getnameinfo、NI_MAXHOST、NI_NUMERICHOST
#include <zookeeper/zookeeper.h>  // Zookeeper C API

using namespace std;

class LoadBalancer {
private:
    vector<pair<string, int>> servers_; // 存储服务器的 IP 和端口
    vector<int> server_sockets_;        // 每个服务器的长连接 socket
    int connection_count_;              // 当前分发的连接计数
    mutex servers_mutex_;               // 保护 servers_ 和 server_sockets_

    string server_zookeeper_hosts_;            // 服务器 Zookeeper 集群地址
    string balancer_zookeeper_hosts_;          // 负载均衡节点 Zookeeper 集群地址
    string root_path_ = "/servers";            // 服务器根节点路径
    string balancer_root_path_ = "/balancers"; // 负载均衡节点根节点路径

    string balancer_ip_; // 负载均衡节点的 IP
    int balancer_port_;  // 负载均衡节点的端口

    zhandle_t *server_zh_;   // 服务器 Zookeeper 句柄
    zhandle_t *balancer_zh_; // 负载均衡节点 Zookeeper 句柄

    bool server_zookeeper_available_;   // 标志服务器 Zookeeper 是否可用
    bool balancer_zookeeper_available_; // 标志负载均衡 Zookeeper 是否可用

public:
    LoadBalancer(const string &server_zookeeper_hosts, const string &balancer_zookeeper_hosts, const string &balancer_ip, int balancer_port)
        : server_zookeeper_hosts_(server_zookeeper_hosts),
          balancer_zookeeper_hosts_(balancer_zookeeper_hosts),
          balancer_ip_(balancer_ip),
          balancer_port_(balancer_port),
          connection_count_(0),
          server_zh_(nullptr),
          balancer_zh_(nullptr),
          server_zookeeper_available_(false),
          balancer_zookeeper_available_(false) {}

    void Start() {
        // 连接到服务器 Zookeeper 集群
        ConnectToServerZookeeper();

        // 连接到负载均衡节点 Zookeeper 集群并注册自身信息
        ConnectToBalancerZookeeper();

        cout << "Load balancer started. Ready to forward requests." << endl;

        // 初始化监听 socket
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
        address.sin_port = htons(balancer_port_);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }

        if (listen(server_fd, SOMAXCONN) < 0) {
            perror("Listen failed");
            exit(EXIT_FAILURE);
        }

        cout << "Load Balancer listening on port " << balancer_port_ << endl;

        socklen_t addrlen = sizeof(address);

        while (true) {
            int client_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
            if (client_socket < 0) {
                perror("Accept failed");
                continue;
            }

            // 创建新线程处理转发逻辑
            thread(&LoadBalancer::ForwardRequest, this, client_socket).detach();
        }
    }

private:
    // 连接到服务器 Zookeeper 集群
    void ConnectToServerZookeeper() {
        server_zh_ = zookeeper_init(server_zookeeper_hosts_.c_str(), nullptr, 30000, nullptr, this, 0);
        if (!server_zh_) {
            cerr << "Failed to connect to server Zookeeper" << endl;
            exit(EXIT_FAILURE);
        }

        // 异步检查根节点是否存在
        zoo_aexists(server_zh_, root_path_.c_str(), 1, ExistsCallback, this);
    }

    // 动态更新服务器列表
    void UpdateServerList() {
        if (!server_zookeeper_available_) {
            cerr << "Server Zookeeper is not available. Cannot update server list." << endl;
            return;
        }

        // 异步获取子节点
        zoo_aget_children(server_zh_, root_path_.c_str(), 1, GetChildrenCallback, this);
    }

    // 与服务器建立长连接
    int ConnectToServer(const string &ip, int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            cerr << "Failed to create socket for server " << ip << ":" << port << endl;
            return -1;
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
            cerr << "Invalid server IP address: " << ip << endl;
            close(sock);
            return -1;
        }

        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            cerr << "Failed to connect to server " << ip << ":" << port << endl;
            close(sock);
            return -1;
        }

        cout << "Connected to server " << ip << ":" << port << endl;
        return sock;
    }

    // 连接到负载均衡节点 Zookeeper 集群
    void ConnectToBalancerZookeeper() {
        balancer_zh_ = zookeeper_init(balancer_zookeeper_hosts_.c_str(), nullptr, 30000, nullptr, this, 0);
        if (!balancer_zh_) {
            cerr << "Failed to connect to balancer Zookeeper" << endl;
            exit(EXIT_FAILURE);
        }

        balancer_zookeeper_available_ = true;
        cout << "Connected to balancer Zookeeper successfully." << endl;

        // 注册负载均衡节点信息
        RegisterBalancerNode();
    }

    // 注册负载均衡节点信息
    void RegisterBalancerNode() {
        string balancer_node_path = balancer_root_path_ + "/" + balancer_ip_ + ":" + to_string(balancer_port_);

        // 异步创建节点
        zoo_acreate(balancer_zh_, balancer_node_path.c_str(), nullptr, -1, &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL, CreateCallback, this);
    }

    // 转发请求
    void ForwardRequest(int client_socket) {
        int server_index;
        int server_socket;

        {
            lock_guard<mutex> lock(servers_mutex_);
            if (servers_.empty()) {
                cerr << "No available servers to handle the request." << endl;
                close(client_socket);
                return;
            }

            server_index = connection_count_ % servers_.size();
            connection_count_++;
            server_socket = server_sockets_[server_index];
        }

        char buffer[1024];
        int valread = read(client_socket, buffer, 1024);
        if (valread > 0) {
            send(server_socket, buffer, valread, 0);
            int valwrite = read(server_socket, buffer, 1024);
            if (valwrite > 0) {
                send(client_socket, buffer, valwrite, 0);
            }
        }

        close(client_socket);
    }

    // Zookeeper 回调函数
    static void ExistsCallback(int rc, const struct Stat *stat, const void *data) {
        if (rc == ZOK) {
            cout << "Node exists." << endl;
        } else if (rc == ZNONODE) {
            cerr << "Node does not exist." << endl;
        } else {
            cerr << "Error checking node existence: " << zerror(rc) << endl;
        }
    }

    static void GetChildrenCallback(int rc, const struct String_vector *strings, const void *data) {
        if (rc != ZOK) {
            cerr << "Failed to get children: " << zerror(rc) << endl;
            return;
        }

        cout << "Children nodes: ";
        for (int i = 0; i < strings->count; ++i) {
            cout << strings->data[i] << " ";
        }
        cout << endl;
    }

    static void CreateCallback(int rc, const char *name, const void *data) {
        if (rc == ZOK) {
            cout << "Node created successfully: " << name << endl;
        } else {
            cerr << "Failed to create node: " << zerror(rc) << endl;
        }
    }
};

// 获取本地非 127.0.0.1 的 IP 地址
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

int main() {
    string server_zookeeper_hosts = "192.168.1.1:2181,192.168.1.2:2181";
    string balancer_zookeeper_hosts = "192.168.248.111:2181,192.168.248.112:2181,192.168.248.113:2181";
    string balancer_ip = GetLocalIPAddress();
    cout<<balancer_ip;
    int balancer_port = 8080;

    //LoadBalancer load_balancer(server_zookeeper_hosts, balancer_zookeeper_hosts, balancer_ip, balancer_port);
    //load_balancer.Start();

    return 0;
}
