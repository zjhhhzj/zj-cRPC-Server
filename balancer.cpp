//g++ balancer.cpp  -o balancer
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

class ServerConnectionManager {
private:
    vector<int> server_ports_;   // 服务器端口列表
    vector<int> server_sockets_; // 服务器 socket 列表，-1 表示不可用
    queue<int> failed_servers_;  // 存储失效服务器的索引
    mutex server_mutex_;         // 服务器列表的互斥锁
    mutex queue_mutex_;          // 队列的互斥锁

    // 控制重连线程
    thread reconnect_thread_;    // 重连线程
    mutex reconnect_mutex_;      // 控制线程启动和停止的互斥锁
    bool reconnect_running_;     // 标志变量，控制线程运行状态
    int queue_count;      // 队列数量

public:
    ServerConnectionManager(const vector<int>& server_ports): server_ports_(server_ports), reconnect_running_(false) {
        server_sockets_.resize(server_ports.size(), -1); // 初始化为 -1 表示不可用
    }

    void Start() {
        // 初始化所有服务器连接
        for (size_t i = 0; i < server_ports_.size(); ++i) {
            if (!ConnectToServer(i)) {
                MarkServerAsFailed(i);
            }
        }
    }

    void StartReconnectProcess() {
        if (!reconnect_running_){
            lock_guard<mutex> lock(reconnect_mutex_);
            if (reconnect_running_) {
                cout << "Reconnect process is already running." << endl;
                return;
            }
            reconnect_running_ = true;
        }else{
            cout << "Reconnect process is already running." << endl;
            return;
        }
        reconnect_thread_ = thread(&ServerConnectionManager::ReconnectServers, this);
        cout << "Reconnect process started." << endl;
    }

    void StopReconnectProcess() {
        {
            lock_guard<mutex> lock(reconnect_mutex_);
            if (!reconnect_running_) {
                cout << "Reconnect process is not running." << endl;
                return;
            }
            reconnect_running_ = false;
        }
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join(); // 等待线程安全退出
        }
        cout << "Reconnect process stopped." << endl;
    }

    int GetServerSocket(size_t index) {
        lock_guard<mutex> lock(server_mutex_);
        return server_sockets_[index];
    }

    size_t GetServerCount() const {
        return server_ports_.size();
    }

    void MarkServerAsFailed(size_t index) {
        {
            lock_guard<mutex> lock(server_mutex_);
            server_sockets_[index] = -1; // 标记为不可用
        }
        {
            lock_guard<mutex> lock(queue_mutex_);
            failed_servers_.push(index); // 将失效服务器加入队列
        }
    }

private:
    bool ConnectToServer(size_t index) {
        int server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            perror("Socket creation failed");
            return false;
        }

        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = inet_addr("127.0.0.1"); // 假设服务器在本地
        server_address.sin_port = htons(server_ports_[index]);

        if (connect(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
            perror("Connection to server failed");
            close(server_socket);
            return false;
        }

        {
            lock_guard<mutex> lock(server_mutex_);
            server_sockets_[index] = server_socket;
        }
        cout << "Connected to server on port " << server_ports_[index] << endl;
        return true;
    }

    void ReconnectServers() {

        while (reconnect_running_) {
            // 如果队列为空但未达到休眠次数，短暂等待再检查
            this_thread::sleep_for(chrono::seconds(10));

            lock_guard<mutex> lock(queue_mutex_);
            if (failed_servers_.empty()) {
                continue;
            }
            queue_count=failed_servers_.size();
            while(queue_count--){
                int failed_index = failed_servers_.front();
                failed_servers_.pop();

                cout << "Attempting to reconnect to server on port " << server_ports_[failed_index] << endl;
                if (ConnectToServer(failed_index)) {
                    cout << "Reconnected to server on port " << server_ports_[failed_index] << endl;
                } else {
                    // 如果重连失败，将其重新放回队列
                    failed_servers_.push(failed_index);
                    this_thread::sleep_for(chrono::seconds(5)); // 等待 5 秒后重试
                }
            }
        }

        cout << "Reconnect thread exiting..." << endl;
    }
};

class LoadBalancer {
private:
    ServerConnectionManager server_connection_manager_; // 服务器连接管理器
    int connection_count_;
    mutex count_mutex_;
public:
    LoadBalancer(const vector<int>& server_ports)
        : server_connection_manager_(server_ports), connection_count_(0) {}

    void Start() {
        // 启动服务器连接管理器
        server_connection_manager_.Start();
        cout << "Load balancer started. Ready to forward requests." << endl;

        // // 模拟客户端请求的处理逻辑
        // while (true) {
        //     int client_socket = AcceptClient();
        //     if (client_socket >= 0) {
        //         thread(&LoadBalancer::ForwardRequest, this, client_socket).detach();
        //     }
        // }
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
    void ForwardRequest(int client_socket) {
        int server_index;
        int server_socket;

        // 确定目标服务器
        {
            lock_guard<mutex> lock(count_mutex_);
            do {
                server_index = connection_count_ % server_connection_manager_.GetServerCount();
                connection_count_++;
            } while ((server_socket = server_connection_manager_.GetServerSocket(server_index)) == -1); // 跳过失效的服务器
        }

        cout << "Forwarding request to server on port " << server_index << endl;

        char buffer[1024];
        int valread = read(client_socket, buffer, 1024);
        if (valread > 0) {
            ssize_t bytes_sent = send(server_socket, buffer, valread, 0);
            if (bytes_sent < 0) {
                perror("Send to server failed");
                server_connection_manager_.MarkServerAsFailed(server_index); // 标记为失效
                close(client_socket);
                return;
            }

            int valwrite = read(server_socket, buffer, 1024);
            if (valwrite > 0) {
                send(client_socket, buffer, valwrite, 0);
            } else if (valwrite <= 0) {
                perror("Read from server failed");
                server_connection_manager_.MarkServerAsFailed(server_index); // 标记为失效
            }
        }

        close(client_socket); // 关闭客户端连接
    }
};

int main() {
    vector<int> server_ports = {5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009};
    LoadBalancer lb(5011, server_ports);
    lb.Start();
    return 0;
}
