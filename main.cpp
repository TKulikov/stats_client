#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define INPUT_ERROR 1
#define FILE_OPEN_ERROR 2
#define PORT_ERROR 3
#define SOCKET_OPEN_ERROR 4
#define CONNECT_ERROR 5
#define RECV_ERROR 6
#define FIFO_ERROR 10

using namespace std;

int main(int argc, char *argv[])
{
    if (argc < 2) {
        cerr << "ERROR: YOU MUST INPUT <server_pid>";
        return INPUT_ERROR;
    }
    const char *FIFO_NAME = "/tmp/scanner_stats_fifo";
    pid_t server_pid = stoi(argv[1]);
    int32_t fd;
    if ((fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK)) == -1) {
        cerr << "ERROR: CAN'T OPEN FIFO FILE";
        return FIFO_ERROR;
    }
    kill(server_pid, SIGUSR1);
    char buffer[4096];
    int attempts = 20;
    bool data_received = false;
    while (attempts > 0) {
        ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            cout << buffer;
            data_received = true;
            attempts = 5;
        } else if (bytes == 0 && data_received) {
            break;
        } else {
            usleep(100000);
            attempts--;
        }
    }
    if (!data_received) {
        cerr << "\nERROR: TIMEOUT. NO DATA FROM SERVER." << endl;
    }
    close(fd);
    return 0;
}
