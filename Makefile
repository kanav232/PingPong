CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f server client
