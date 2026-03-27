#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <set>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define INPUT_ERROR 1
#define FILE_OPEN_ERROR 2
#define PORT_ERROR 3
#define SOCKET_OPEN_ERROR 4
#define CONNECT_ERROR 5
#define RECV_ERROR 6
#define BIND_ERROR 7

const size_t MAX_PATTERNS = 4096;
const char *FIFO_NAME = "/tmp/scanner_stats_fifo";

using namespace std;
struct Stats
{
    atomic_size_t total_count;
    atomic_size_t found_count[MAX_PATTERNS];
};
Stats *stats;
vector<string> patterns_names;
int32_t server_fd;
int32_t server_pid;

struct Node
{
    unique_ptr<Node> next[256];
    Node *fail = nullptr;
    bool is_terminal = false;
    size_t pattern = -1;
    Node()
    {
        for (int i = 0; i < 256; ++i) {
            next[i] = nullptr;
        }
    }
};

void build_trie(Node *root, const string &path)
{
    ifstream file(path);
    string line;
    size_t id = 0;
    while (getline(file, line)) {
        if (line.empty())
            continue;
        Node *curr = root;
        for (char c : line) {
            if (!curr->next[(uint8_t) c])
                curr->next[(uint8_t) c] = make_unique<Node>();
            curr = curr->next[(uint8_t) c].get();
        }
        curr->is_terminal = true;
        curr->pattern = id;
        ++id;
        patterns_names.push_back(line);
    }
    queue<Node *> queue;
    for (int i = 0; i < 256; ++i) {
        if (root->next[i]) {
            root->next[i]->fail = root;
            queue.push(root->next[i].get());
        }
    }
    while (!queue.empty()) {
        Node *node = queue.front();
        queue.pop();
        for (int i = 0; i < 256; ++i) {
            if (node->next[i]) {
                Node *v = node->next[i].get();
                Node *f = node->fail;
                while (f != root && !f->next[i])
                    f = f->fail;
                v->fail = f->next[i] ? f->next[i].get() : root;
                v->is_terminal |= v->fail->is_terminal;
                queue.push(v);
            }
        }
    }
}

void client_handle(uint16_t client_sock, Node *root)
{
    stats->total_count.fetch_add(1, memory_order_relaxed);
    Node *curr = root;
    size_t bytes;
    char buffer[4096];
    set<size_t> detected_ids;
    while ((bytes = recv(client_sock, buffer, sizeof(buffer), 0)) > 0) {
        for (size_t i = 0; i < bytes; ++i) {
            uint8_t c = (uint8_t) buffer[i];
            while (curr != root && !curr->next[c])
                curr = curr->fail;
            if (curr->next[c])
                curr = curr->next[c].get();
            if (curr->is_terminal) {
                Node *tmp = curr;
                while (tmp != root && tmp->is_terminal) {
                    if (tmp->pattern != -1) {
                        stats->found_count[tmp->pattern]++;
                        detected_ids.insert(tmp->pattern);
                    }
                    tmp = tmp->fail;
                }
            }
        }
    }
    string response;
    if (detected_ids.empty())
        response = "STATUS: OK.\n THREADS NOT FOUND";
    else {
        response = "STATUS: INFECTED.\n FOUNDED THREATS:\n";
        for (auto &id : detected_ids) {
            response += patterns_names[id] + "\n";
        }
    }
    send(client_sock, response.c_str(), response.length(), 0);
    close(client_sock);
    exit(0);
}

void get_stats()
{
    int32_t fd = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        cerr << "ERROR: CAN'T OPEN FIFO FILE";
    }
    string report;
    report += "\n=== Global Statistics ===\n";
    report += "TOTAL FILE SCANNED: " + to_string(stats->total_count);
    report += "PATTERNS FOUND:\n";
    for (size_t i = 0; i < patterns_names.size(); ++i) {
        report += patterns_names[i] + " -> " + to_string(stats->found_count[i]) + '\n';
    }
    report += "=========================\n";
    write(fd, report.c_str(), report.length());
    close(fd);
}

void signal_handler(int sig) //int , так как int32_t ломал std::signal
{
    if (getpid() != server_pid)
        return;
    if (sig == SIGUSR1) {
        get_stats();
    } else {
        cout << "\nTerminating server..." << endl;
        close(server_fd);
        while (wait(NULL) > 0)
            ;
        exit(0);
    }
}

int main(int argc, char *argv[])
{
    server_pid = getpid();
    mkfifo(FIFO_NAME, 0666);
    if (argc < 3) {
        cerr << "ERROR: You must input <file> <port>" << endl;
        return INPUT_ERROR;
    }
    unique_ptr<Node> root = make_unique<Node>();
    stats = (Stats *)
        mmap(NULL, sizeof(Stats), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(stats, 0, sizeof(Stats));
    build_trie(root.get(), argv[1]);
    uint16_t port = stoi(argv[2]);
    if (!port) {
        cerr << "ERROR: PORT MUST BE INTEGER";
        return PORT_ERROR;
    }
    if (!(server_fd = socket(AF_INET, SOCK_STREAM, 0))) {
        cerr << "ERROR: CAN'T OPEN SOCKET";
        return SOCKET_OPEN_ERROR;
    }
    int32_t opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        cerr << "ERROR: CAN'T BIND SOCKET";
        return BIND_ERROR;
    }
    listen(server_fd, 100);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);
    cout << "Server PID: " << server_pid << " listening on " << port << endl;
    while (true) {
        int32_t client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;
        if (fork() == 0) {
            client_handle(client_fd, root.get());
        } else {
            close(client_fd);
            while (waitpid(-1, nullptr, WNOHANG) > 0)
                ;
        }
    }
    return 0;
}
