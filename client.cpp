// client.cpp
// C++ UDP ASCII air-hockey client
// Compile: g++ -std=c++17 -O2 -pthread client.cpp -o client
#include <vector>
#include <cmath> 
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <termios.h>
#include <fcntl.h>

using namespace std::chrono;

const char *SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 40000;
const int FIELD_W = 80;
const int FIELD_H = 24;
const float PADDLE_H = 4.0f;

#pragma pack(push,1)
struct InputMsg {
    uint8_t type; // 1 join, 2 input
    uint8_t dir;  // 0 none,1 up,2 down
    uint32_t seq;
};
struct StateMsg {
    uint8_t type; // 10
    uint8_t your_id;
    uint16_t tick;
    float puck_x, puck_y;
    float puck_vx, puck_vy;
    float pad1_y, pad2_y;
    uint8_t score1, score2;
};
#pragma pack(pop)

int sockfd;
sockaddr_in servaddr;
std::atomic<int> curDir{0}; // 0 none,1 up,2 down
std::atomic<bool> running{true};
StateMsg latestState{};
std::atomic<bool> stateReceived{false};

void set_nonblocking_stdin() {
    termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~(ICANON | ECHO); // raw mode
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

void clear_terminal() {
    std::cout << "\x1b[2J\x1b[H";
}

void draw_ascii(const StateMsg &s) {
    clear_terminal();
    int w = FIELD_W, h = FIELD_H;
    std::vector<std::string> screen(h, std::string(w, ' '));
    // borders
    for (int x=0;x<w;x++){ screen[0][x] = '-'; screen[h-1][x] = '-'; }
    // draw puck
    int px = std::max(1, std::min(w-2, (int)std::round(s.puck_x)));
    int py = std::max(1, std::min(h-2, (int)std::round(s.puck_y)));
    screen[py][px] = 'O';
    // paddles (vertical)
    int pad1c = (int)std::round(s.pad1_y);
    int pad2c = (int)std::round(s.pad2_y);
    for (int dy = -((int)PADDLE_H/2); dy <= (int)PADDLE_H/2; dy++) {
        int y1 = pad1c + dy;
        int y2 = pad2c + dy;
        if (y1>=1 && y1<h-1) screen[y1][2] = '|';
        if (y2>=1 && y2<h-1) screen[y2][w-3] = '|';
    }
    // draw center line
    for (int y=1;y<h-1;y+=1) if (y%2==0) screen[y][w/2] = '|';
    // print screen
    for (auto &row : screen) std::cout << row << "\n";
    std::cout << "You are player " << (int)s.your_id << "    Score: " << (int)s.score1 << " - " << (int)s.score2 << "\n";
    std::cout << "Controls: w = up, s = down, q = quit\n";
}

void recv_thread_func() {
    while (running) {
        StateMsg st;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(sockfd, &st, sizeof(st), 0, (sockaddr*)&from, &fl);
        if (r <= 0) { std::this_thread::sleep_for(milliseconds(1)); continue; }
        if (st.type == 10) {
            latestState = st;
            stateReceived = true;
            draw_ascii(latestState);
        }
    }
}

int main(int argc, char **argv) {
    const char* server_ip = SERVER_IP;
    if (argc >= 2) server_ip = argv[1];
    set_nonblocking_stdin();

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    inet_aton(server_ip, &servaddr.sin_addr);

    // send join
    InputMsg join{};
    join.type = 1;
    sendto(sockfd, &join, sizeof(join), 0, (sockaddr*)&servaddr, sizeof(servaddr));

    std::thread recv_t(recv_thread_func);
    recv_t.detach();

    uint32_t seq = 0;
    while (running) {
        // read keyboard
        char c=0;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r>0) {
            if (c == 'w') curDir = 1;
            else if (c == 's') curDir = 2;
            else if (c == 'q') { running=false; break; }
            else if (c == '\n') curDir = 0;
        } else {
            // if no key pressed, send no-move (you could keep last)
            // For a simpler feel, we send 0 to stop movement
            curDir = 0;
        }

        // send input as often as 30Hz
        InputMsg in{};
        in.type = 2;
        in.dir = (uint8_t)curDir.load();
        in.seq = seq++;
        sendto(sockfd, &in, sizeof(in), 0, (sockaddr*)&servaddr, sizeof(servaddr));

        std::this_thread::sleep_for(milliseconds(33));
    }

    close(sockfd);
    // restore terminal defaults (best-effort)
    termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return 0;
}
