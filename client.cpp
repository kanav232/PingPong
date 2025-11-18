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
#include <string>
#include <thread>
#include <vector>

#include <termios.h>
#include <fcntl.h>

using namespace std;
using namespace chrono;

constexpr int SERVER_PORT = 5000;

constexpr int W = 80;
constexpr int H = 24;
constexpr float PADDLE_H = 4.0f;

constexpr int TICKS_PER_SEC = 60;
constexpr float FRAME_DT = 1.0f / 60.0f;

#pragma pack(push,1)
struct InputMsg {
    uint8_t type;  // 1=JOIN, 2=INPUT
    uint8_t dir;   // 0 stop,1 up,2 down
    uint32_t seq;
};
struct AssignMsg {
    uint8_t type;   // 5
    uint8_t player_id;
};
struct StateMsg {
    uint8_t type;
    uint8_t your_id;
    uint32_t tick;
    float puck_x, puck_y;
    float puck_vx, puck_vy;
    float pad1_y, pad2_y;
    uint8_t score1, score2;
    uint8_t gameOver;
};
#pragma pack(pop)

int sockfd;
sockaddr_in serv{};

atomic<uint32_t> seqCounter{1};
atomic<bool> running{true};
atomic<bool> gotState{false};

mutex st_mtx;
StateMsg prevS{}, lastS{};
steady_clock::time_point lastRecv;
int myID = 0;

// Terminal utilities
void raw_on() {
    termios t;
    tcgetattr(STDIN_FILENO,&t);
    t.c_lflag &= ~(ICANON|ECHO);
    tcsetattr(STDIN_FILENO,TCSANOW,&t);
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
}
void raw_off() {
    termios t;
    tcgetattr(STDIN_FILENO,&t);
    t.c_lflag |= (ICANON|ECHO);
    tcsetattr(STDIN_FILENO,TCSANOW,&t);
}
void cls() {
    cout << "\033[2J\033[H";
}
void hide() { cout << "\033[?25l"; }
void show() { cout << "\033[?25h"; }

float LERP(float a,float b,float t){return a+(b-a)*t;}

void draw(const StateMsg &A, const StateMsg &B, float alpha) {
    float px = LERP(A.puck_x, B.puck_x, alpha);
    float py = LERP(A.puck_y, B.puck_y, alpha);
    float p1 = LERP(A.pad1_y, B.pad1_y, alpha);
    float p2 = LERP(A.pad2_y, B.pad2_y, alpha);

    cls();
    cout << "=== PingPong Arena ===   You are Player " << B.your_id << "\n";
    cout << "Score: " << (int)B.score1 << " - " << (int)B.score2 << "\n";

    vector<string> scr(H, string(W, ' '));

    for (int x=0;x<W;x++){
        scr[0][x]='#';
        scr[H-1][x]='#';
    }

    int p1_top = (int)round(p1 - PADDLE_H/2);
    int p2_top = (int)round(p2 - PADDLE_H/2);

    for (int d=0; d<(int)PADDLE_H; d++){
        int y1=p1_top+d;
        int y2=p2_top+d;
        if(y1>0&&y1<H-1) scr[y1][2]='|';
        if(y2>0&&y2<H-1) scr[y2][W-3]='|';
    }

    int ix = (int)round(px);
    int iy = (int)round(py);
    if(ix>=0&&ix<W&&iy>=0&&iy<H) scr[iy][ix]='O';

    for(int y=1;y<H-1;y++)
        if(y%2==0) scr[y][W/2]='|';

    for (auto &r: scr) cout << r << "\n";

    if (B.gameOver == 1) cout << "\nPLAYER 1 WINS!\n";
    if (B.gameOver == 2) cout << "\nPLAYER 2 WINS!\n";
}

void recv_thread() {
    while (running) {
        uint8_t buf[256];
        sockaddr_in src{};
        socklen_t sl=sizeof(src);
        ssize_t r=recvfrom(sockfd,buf,sizeof(buf),0,(sockaddr*)&src,&sl);
        if (r<=0){ this_thread::sleep_for(1ms); continue; }

        uint8_t type=buf[0];

        if(type==5 && r>=2){
            AssignMsg a{};
            memcpy(&a,buf,sizeof(a));
            myID=a.player_id;
            cerr<<"[client] Assigned as Player "<<myID<<"\n";
        }
        else if(type==10 && r>= (ssize_t)sizeof(StateMsg)){
            StateMsg s{};
            memcpy(&s,buf,sizeof(s));

            lock_guard<mutex> g(st_mtx);
            prevS=lastS;
            lastS=s;
            gotState=true;
            lastRecv=steady_clock::now();
        }
    }
}

int main(int argc, char**argv){
    cout<<"=== PingPong Arena Client ===\n";

    string ip = (argc>=2 ? argv[1] : "127.0.0.1");

    sockfd=socket(AF_INET,SOCK_DGRAM,0);

    serv.sin_family=AF_INET;
    serv.sin_port=htons(SERVER_PORT);
    inet_pton(AF_INET, ip.c_str(), &serv.sin_addr);

    // SEND JOIN
    InputMsg j{};
    j.type=1;
    j.dir=0;
    j.seq=seqCounter++;
    sendto(sockfd,&j,sizeof(j),0,(sockaddr*)&serv,sizeof(serv));
    cerr<<"[client] JOIN sent\n";

    thread rt(recv_thread);
    rt.detach();

    // WAIT FOR PLAYER ASSIGNMENT
    int ms=0;
    while(myID==0 && ms<5000){
        this_thread::sleep_for(10ms);
        ms+=10;
    }
    if(myID==0){
        cerr<<"No assignment from server.\n";
        return 1;
    }

    raw_on();
    hide();

    auto next=steady_clock::now();
    uint8_t dir=0;

    while(running){
        next += milliseconds(1000/60);

        int ch=getchar();
        if(ch!=EOF){
            if(ch=='q'){ running=false; break; }
            if(ch=='w'||ch=='W')dir=1;
            else if(ch=='s'||ch=='S')dir=2;
            else dir=0;
        } else dir=0;

        InputMsg im{};
        im.type=2;
        im.dir=dir;
        im.seq=seqCounter++;
        sendto(sockfd,&im,sizeof(im),0,(sockaddr*)&serv,sizeof(serv));

        if(gotState){
            StateMsg A,B;
            {
                lock_guard<mutex>g(st_mtx);
                A=prevS;
                B=lastS;
            }

            float alpha=1.0f;
            if(A.tick!=0 && B.tick>A.tick){
                auto now=steady_clock::now();
                float ms = duration_cast<milliseconds>(now-lastRecv).count();
                alpha = min(1.0f, ms/(1000.0f/TICKS_PER_SEC));
            }

            draw(A,B,alpha);

            if(B.gameOver!=0){
                show();
                raw_off();
                cerr<<"Game over. Exiting in 5s...\n";
                sleep(5);
                break;
            }
        }
        else {
            cls();
            cout<<"Waiting for server state...\n";
        }

        this_thread::sleep_until(next);
    }

    show();
    raw_off();
    close(sockfd);
    return 0;
}
