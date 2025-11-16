// server.cpp
// C++ UDP authoritative air-hockey server (ASCII clients)
// Compile: g++ -std=c++17 -O2 -pthread server.cpp -o server
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <mutex>

using namespace std::chrono;

const int PORT = 40000;
const int TICKS_PER_SEC = 60;
const float DT = 1.0f / TICKS_PER_SEC;

const int FIELD_W = 80;
const int FIELD_H = 24;
const float PADDLE_H = 4.0f;
const float PADDLE_SPEED = 20.0f; // units per second
const float PUCK_SPEED_INIT = 20.0f;

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

struct PlayerAddr {
    sockaddr_in addr;
    bool valid=false;
};

int sockfd;
PlayerAddr players[2];
std::mutex addr_mtx;

std::atomic<uint16_t> server_tick{0};

float padY[2];
float puck_x, puck_y, puck_vx, puck_vy;
uint8_t score1=0, score2=0;

void bind_socket() {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(PORT);
    if (bind(sockfd, (sockaddr*)&serv, sizeof(serv)) < 0) { perror("bind"); exit(1); }
    // make non-blocking if desired: but we'll use recvfrom with MSG_DONTWAIT occasionally
}

void handle_packets() {
    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        InputMsg in;
        ssize_t r = recvfrom(sockfd, &in, sizeof(in), MSG_DONTWAIT, (sockaddr*)&cli, &len);
        if (r <= 0) { std::this_thread::sleep_for(milliseconds(1)); continue; }
        if (in.type == 1) { // JOIN
            std::lock_guard<std::mutex> g(addr_mtx);
            // assign to first empty slot
            for (int i=0;i<2;i++){
                if (!players[i].valid) {
                    players[i].addr = cli;
                    players[i].valid = true;
                    std::cout<<"Assigned player "<<(i+1)<<" to "<<inet_ntoa(cli.sin_addr)<<":"<<ntohs(cli.sin_port)<<"\n";
                    break;
                }
            }
        } else if (in.type == 2) {
            // find which player
            std::lock_guard<std::mutex> g(addr_mtx);
            for (int i=0;i<2;i++){
                if (players[i].valid &&
                    players[i].addr.sin_addr.s_addr == cli.sin_addr.s_addr &&
                    players[i].addr.sin_port == cli.sin_port) {
                    // apply direct immediate movement influence (store dir into padY control array)
                    float dy = 0.0f;
                    if (in.dir == 1) dy = -PADDLE_SPEED;
                    else if (in.dir == 2) dy = PADDLE_SPEED;
                    // we'll integrate in physics loop by storing desired speed in padY as velocity
                    // But to keep it simple: we attach dir into padY by a small velocity mark in a secondary array
                    // We'll use padYVel to hold last input influence (in a real build you may queue inputs)
                    // For simplicity, store dir in seq field? No—use a small table:
                    // For this starter, we'll simply set a signed flag in seq (not ideal) — instead use global flags:
                    // We'll implement padDir array (0/1/2)
                }
            }
        }
    }
}

// We'll store last direction per player in padDir
int padDir[2] = {0,0};

void packet_receiver_thread() {
    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        uint8_t buf[64];
        ssize_t r = recvfrom(sockfd, buf, sizeof(buf), 0, (sockaddr*)&cli, &len);
        if (r <= 0) { std::this_thread::sleep_for(milliseconds(1)); continue; }
        InputMsg in;
        if (r >= (ssize_t)sizeof(InputMsg)) memcpy(&in, buf, sizeof(InputMsg));
        else continue;
        if (in.type == 1) {
            std::lock_guard<std::mutex> g(addr_mtx);
            for (int i=0;i<2;i++){
                if (!players[i].valid) {
                    players[i].addr = cli;
                    players[i].valid = true;
                    std::cout << "Player " << (i+1) << " joined from " << inet_ntoa(cli.sin_addr) << ":" << ntohs(cli.sin_port) << "\n";
                    break;
                }
            }
        } else if (in.type == 2) {
            std::lock_guard<std::mutex> g(addr_mtx);
            for (int i=0;i<2;i++){
                if (players[i].valid &&
                    players[i].addr.sin_addr.s_addr == cli.sin_addr.s_addr &&
                    players[i].addr.sin_port == cli.sin_port) {
                    padDir[i] = in.dir;
                }
            }
        }
    }
}

void send_state_to_all() {
    StateMsg st{};
    st.type = 10;
    st.tick = server_tick.load();
    st.puck_x = puck_x;
    st.puck_y = puck_y;
    st.puck_vx = puck_vx;
    st.puck_vy = puck_vy;
    st.pad1_y = padY[0];
    st.pad2_y = padY[1];
    st.score1 = score1;
    st.score2 = score2;
    // send separately to each valid player, setting your_id appropriately
    std::lock_guard<std::mutex> g(addr_mtx);
    for (int i=0;i<2;i++){
        if (!players[i].valid) continue;
        st.your_id = (uint8_t)(i+1);
        sendto(sockfd, &st, sizeof(st), 0, (sockaddr*)&players[i].addr, sizeof(players[i].addr));
    }
}

void reset_positions() {
    padY[0] = FIELD_H/2.0f;
    padY[1] = FIELD_H/2.0f;
    puck_x = FIELD_W/2.0f;
    puck_y = FIELD_H/2.0f;
    // initial velocity to a random side
    puck_vx = (rand()%2?1:-1) * PUCK_SPEED_INIT;
    puck_vy = ((rand()%200)-100)/100.0f * 5.0f;
}

int main(){
    srand(time(nullptr));
    bind_socket();
    std::thread recv_t(packet_receiver_thread);
    recv_t.detach();

    // initialize
    padY[0] = FIELD_H/2.0f;
    padY[1] = FIELD_H/2.0f;
    reset_positions();

    auto next_tick = high_resolution_clock::now();

    while (true) {
        next_tick += milliseconds(1000 / TICKS_PER_SEC);
        // integrate inputs into paddles
        for (int i=0;i<2;i++){
            float vel = 0.0f;
            if (padDir[i] == 1) vel = -PADDLE_SPEED;
            else if (padDir[i] == 2) vel = PADDLE_SPEED;
            padY[i] += vel * DT;
            // clamp
            if (padY[i] < PADDLE_H/2.0f) padY[i] = PADDLE_H/2.0f;
            if (padY[i] > FIELD_H - PADDLE_H/2.0f) padY[i] = FIELD_H - PADDLE_H/2.0f;
        }

        // update puck
        puck_x += puck_vx * DT;
        puck_y += puck_vy * DT;

        // top/bottom bounce
        if (puck_y < 0.5f) { puck_y = 0.5f; puck_vy = -puck_vy; }
        if (puck_y > FIELD_H - 0.5f) { puck_y = FIELD_H - 0.5f; puck_vy = -puck_vy; }

        // left paddle collision (x near 2)
        if (puck_x <= 2.5f) {
            float rel = puck_y - padY[0];
            if (std::abs(rel) <= PADDLE_H/2.0f) {
                // reflect with slight influence
                puck_x = 2.5f;
                puck_vx = -puck_vx;
                puck_vy += rel * 2.0f; // add spin
            } else {
                // goal to right
                score2++;
                reset_positions();
            }
        }

        // right paddle collision (x near FIELD_W-2)
        if (puck_x >= FIELD_W - 2.5f) {
            float rel = puck_y - padY[1];
            if (std::abs(rel) <= PADDLE_H/2.0f) {
                puck_x = FIELD_W - 2.5f;
                puck_vx = -puck_vx;
                puck_vy += rel * 2.0f;
            } else {
                // goal to left
                score1++;
                reset_positions();
            }
        }

        // small friction to avoid runaway
        puck_vx *= 1.0f;
        puck_vy *= 0.999f;

        // increment tick and send state
        server_tick++;
        send_state_to_all();

        std::this_thread::sleep_until(next_tick);
    }

    close(sockfd);
    return 0;
}
