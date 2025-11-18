#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
using namespace std;
using namespace chrono;

constexpr int SERVER_PORT = 5000;
constexpr int TICKS_PER_SEC = 60;
constexpr float DT = 1.0f / TICKS_PER_SEC;

constexpr int W = 80;
constexpr int H = 24;
constexpr float PADDLE_H = 4.0f;
constexpr float PADDLE_SPEED = 26.0f;
constexpr float PUCK_SPEED = 25.0f;
constexpr int MAX_SCORE = 20;

#pragma pack(push,1)
struct InputMsg {
    uint8_t type;  // 1 = JOIN, 2 = INPUT
    uint8_t dir;   // 0 stop, 1 up, 2 down
    uint32_t seq;
};
struct AssignMsg {
    uint8_t type;      // 5
    uint8_t player_id; // 1 or 2
};
struct StateMsg {
    uint8_t type;  
    uint8_t your_id;
    uint32_t tick;

    float puck_x, puck_y;
    float puck_vx, puck_vy;

    float pad1_y, pad2_y;

    uint8_t score1, score2;
    uint8_t gameOver; // 0=playing 1=P1 won 2=P2 won
};
#pragma pack(pop)

struct Player {
    bool active = false;
    sockaddr_in addr{};
    uint8_t dir = 0;
    uint32_t last_seq = 0;
};

Player P[2];
int sockfd;
mutex mtx;

atomic<uint32_t> TICK{0};

// GAME STATE
float padY[2];
float puck_x, puck_y, puck_vx, puck_vy;
uint8_t score1 = 0, score2 = 0;
uint8_t gameOver = 0;

mt19937 rng((unsigned)time(nullptr));

void reset_ball(int dir) {
    puck_x = W/2;
    puck_y = H/2;
    puck_vx = PUCK_SPEED * dir;
    puck_vy = (uniform_real_distribution<float>(-1.2,1.2))(rng);
}

void send_assign(Player &pl) {
    AssignMsg a{5, pl.active ? (pl.addr.sin_port?1:2) : 0};
    // actually correct id from index
}

void recv_thread() {
    while (true) {
        uint8_t buf[64];
        sockaddr_in cli{};
        socklen_t sl = sizeof(cli);
        ssize_t r = recvfrom(sockfd, buf, sizeof(buf), 0, (sockaddr*)&cli, &sl);
        if (r <= 0) { this_thread::sleep_for(1ms); continue; }

        if (r < (ssize_t)sizeof(InputMsg)) continue;

        InputMsg in;
        memcpy(&in, buf, sizeof(in));

        if (in.type == 1) { // JOIN
            lock_guard<mutex> g(mtx);
            for (int i=0;i<2;i++) {
                if (!P[i].active) {
                    P[i].active = true;
                    P[i].addr = cli;
                    P[i].dir = 0;
                    P[i].last_seq = 0;

                    AssignMsg a{5, (uint8_t)(i+1)};
                    sendto(sockfd, &a, sizeof(a), 0, (sockaddr*)&cli, sl);
                    cerr << "[server] Assigned player " << (i+1)
                         << " (" << inet_ntoa(cli.sin_addr) << ":" << ntohs(cli.sin_port) << ")\n";
                    break;
                }
            }
        }
        else if (in.type == 2) { // INPUT
            lock_guard<mutex> g(mtx);
            for (int i=0;i<2;i++) {
                if (P[i].active &&
                    P[i].addr.sin_addr.s_addr == cli.sin_addr.s_addr &&
                    P[i].addr.sin_port == cli.sin_port) {

                    if (in.seq > P[i].last_seq) {
                        P[i].last_seq = in.seq;
                        P[i].dir = in.dir;
                    }
                }
            }
        }
    }
}

void send_state() {
    lock_guard<mutex> g(mtx);
    for (int i=0;i<2;i++) {
        if (!P[i].active) continue;

        StateMsg s{};
        s.type = 10;
        s.your_id = i+1;
        s.tick = TICK.load();

        s.puck_x = puck_x;
        s.puck_y = puck_y;
        s.puck_vx = puck_vx;
        s.puck_vy = puck_vy;

        s.pad1_y = padY[0];
        s.pad2_y = padY[1];

        s.score1 = score1;
        s.score2 = score2;
        s.gameOver = gameOver;

        sendto(sockfd, &s, sizeof(s), 0,
               (sockaddr*)&P[i].addr, sizeof(P[i].addr));
    }
}

int main() {
    cout << "=== PingPong Arena Server ===\n";

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(SERVER_PORT);
    serv.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (sockaddr*)&serv, sizeof(serv));

    thread t(recv_thread);
    t.detach();

    cout << "Waiting for 2 players...\n";
    while (!(P[0].active && P[1].active)) 
        this_thread::sleep_for(50ms);

    cout << "Players ready. Game starting...\n";

    padY[0] = padY[1] = H/2;
    reset_ball((uniform_int_distribution<int>(0,1)(rng)?1:-1));

    auto next = steady_clock::now();

    while (true) {
        next += milliseconds(1000/TICKS_PER_SEC);

        // update paddles
        {
            lock_guard<mutex> g(mtx);
            for (int i=0;i<2;i++) {
                if (P[i].dir == 1) padY[i] -= PADDLE_SPEED * DT;
                if (P[i].dir == 2) padY[i] += PADDLE_SPEED * DT;

                float half = PADDLE_H/2;
                if (padY[i] < half) padY[i] = half;
                if (padY[i] > H-1-half) padY[i] = H-1-half;
            }
        }

        // update puck
        puck_x += puck_vx * DT;
        puck_y += puck_vy * DT;

        if (puck_y < 1) { puck_y=1; puck_vy=-puck_vy; }
        if (puck_y > H-2) { puck_y=H-2; puck_vy=-puck_vy; }

        // paddle collisions
        // left
        if (puck_x <= 3) {
            if (fabs(puck_y - padY[0]) <= PADDLE_H/2 + 0.5f) {
                puck_x=3;
                puck_vx = fabs(puck_vx);
            } else {
                score2++;
                if (score2 >= MAX_SCORE) { gameOver = 2; }
                reset_ball(1);
            }
        }
        // right
        if (puck_x >= W-4) {
            if (fabs(puck_y - padY[1]) <= PADDLE_H/2 + 0.5f) {
                puck_x=W-4;
                puck_vx = -fabs(puck_vx);
            } else {
                score1++;
                if (score1 >= MAX_SCORE) { gameOver = 1; }
                reset_ball(-1);
            }
        }

        TICK++;
        send_state();

        if (gameOver != 0) break;

        this_thread::sleep_until(next);
    }

    cerr << "Game finished. Winner = Player " << (int)gameOver << "\n";

    // send final state for a bit
    for (int i=0;i<120;i++) {
        send_state();
        this_thread::sleep_for(16ms);
    }

    close(sockfd);
    return 0;
}
