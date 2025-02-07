//g++ why.cpp -o why -lzookeeper_mt -pthread
#include <iostream>
#include <zookeeper/zookeeper.h>
#include <unistd.h>
#include <csignal>

using namespace std;

// ZooKeeper 全局变量
zhandle_t* zk_handle = nullptr;
const char* zk_servers = "192.168.248.111:2181,192.168.248.112:2181,192.168.248.113:2181";
const char* zk_root_path = "/servers";

// 处理 ZooKeeper 连接状态的 Watcher
void Watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {
    if (type == ZOO_SESSION_EVENT && state == ZOO_CONNECTED_STATE) {
        cout << "ZooKeeper connection established." << endl;
    } else if (state == ZOO_EXPIRED_SESSION_STATE) {
        cerr << "ZooKeeper session expired." << endl;
    }
}

// 确保根路径存在
void EnsureRootPathExists() {
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

int main() {
    // 捕获 Ctrl+C 信号，确保关闭 ZooKeeper 连接
    signal(SIGINT, [](int) {
        if (zk_handle) {
            zookeeper_close(zk_handle);
        }
        exit(0);
    });

    // 初始化 ZooKeeper 连接
    zk_handle = zookeeper_init(zk_servers, Watcher, 30000, 0, nullptr, 0);
    if (!zk_handle) {
        cerr << "Failed to connect to ZooKeeper" << endl;
        return EXIT_FAILURE;
    }

    // 确保 /servers 路径存在
    EnsureRootPathExists();

    // 注册服务节点
    string node_path = string(zk_root_path) + "/node_5000";
    string node_data = "192.168.248.128:5000";

    if(fork()>0){
        exit(0);
    }else{
        RegisterToZooKeeperAsync(node_path, node_data);
    }
    // 保持程序运行，等待回调
    while (true) {
        pause();
    }

    return 0;
}