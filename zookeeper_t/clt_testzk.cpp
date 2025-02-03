//g++ clt_testzk.cpp -o clt_testzk -lzookeeper_mt
#include <zookeeper/zookeeper.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <condition_variable>

using namespace std;

// 全局变量，用于存储负载均衡节点
vector<string> load_balancer_nodes;
mutex mtx;
condition_variable cv;
bool is_done = false;

// 回调函数，用于处理异步获取子节点的结果
void get_children_callback(int rc, const struct String_vector* strings, const void* data) {
    if (rc != ZOK) {
        cerr << "Error getting children asynchronously: " << zerror(rc) << endl;
        return;
    }

    // 锁定全局变量并更新节点列表
    {
        lock_guard<mutex> lock(mtx);
        load_balancer_nodes.clear();
        for (int i = 0; i < strings->count; ++i) {
            load_balancer_nodes.emplace_back(strings->data[i]);
        }
        is_done = true;
    }

    // 通知主线程操作完成
    cv.notify_one();
}

// 连接状态变化的回调函数
void watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {
    if (type == ZOO_SESSION_EVENT && state == ZOO_CONNECTED_STATE) {
        cout << "Zookeeper connected successfully!" << endl;
    } else if (type == ZOO_SESSION_EVENT && state == ZOO_EXPIRED_SESSION_STATE) {
        cerr << "Zookeeper session expired!" << endl;
    }
}

int main() {
    // Zookeeper 集群地址
    string zookeeper_hosts = "192.168.248.111:2181,192.168.248.112:2181,192.168.248.113:2181";

    // 连接到 Zookeeper
    zhandle_t* zh = zookeeper_init(zookeeper_hosts.c_str(), watcher, 30000, nullptr, nullptr, 0);
    if (!zh) {
        cerr << "Failed to connect to Zookeeper" << endl;
        return -1;
    }

    // 异步获取负载均衡节点
    string path = "/load_balancers";
    int ret = zoo_aget_children(zh, path.c_str(), 0, get_children_callback, nullptr);
    if (ret != ZOK) {
        cerr << "Error initiating async get children: " << zerror(ret) << endl;
        zookeeper_close(zh);
        return -1;
    }

    // 等待异步操作完成
    {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [] { return is_done; });
    }

    // 打印所有负载均衡节点
    cout << "Available load balancer nodes:" << endl;
    for (const auto& node : load_balancer_nodes) {
        cout << node << endl;
    }

    // 关闭 Zookeeper 连接
    zookeeper_close(zh);
    return 0;
}

