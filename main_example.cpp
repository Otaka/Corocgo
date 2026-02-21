//  main.cpp
//  MiniCoroTest

#include <stdio.h>
#include "minicoro.h"
#include "corocgo.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <string>

using namespace std;
using namespace corocgo;

void _main() {
    //everything is running in one thread
    
    auto channel = makeChannel<int>(1);
    
    coro([channel]() {
        int counter=0;
        while(true) {
            sleep(500);
            if(!channel->send(counter++))
                break;
        }
    });
    
    coro([channel]() {
        int counter=1000000;
        while(true) {
            sleep(1000);
            if(!channel->send(counter++))
                break;
        }
    });
    
    coro([channel]() {
        while(true) {
            auto [data, error] = channel->receive();
            if(error)
                break;
            printf("Data received %d\n", data);
            if(data==20)channel->close();
        }
    });
}

int main(int argc, const char * argv[]) {
    coro(_main);
    scheduler_start();
    printf("Finish\n");
}
