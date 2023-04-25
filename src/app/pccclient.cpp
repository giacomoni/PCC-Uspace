#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <iostream>
#include "../core/udt.h"
#include "../core/options.h"
#include <signal.h>

#define DATA_BATCH_SIZE 1000000 // 1MB

using namespace std;

struct arg_struct
{
    void *s;
    int perf_interval;
    int duration;
};

void *monitor(void *);
void *send_ctr(void *);

UDTSOCKET client;
bool stop = false;

uint64_t raw_timestamp(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t us = ts.tv_nsec / 1000;
    us += (uint64_t)ts.tv_sec * 1000000;
    return us;
}

uint64_t initial_timestamp(void)
{
    static uint64_t initial_value = raw_timestamp();
    return initial_value;
}

uint64_t timestamp(void)
{
    return raw_timestamp() - initial_timestamp();
}

void intHandler(int dummy)
{
    stop = true;
}

void prusage()
{
    cerr << "usage: .../pccclient <send|recv> server_ip server_port perf_interval duration [OPTIONS]" << endl;
    exit(-1);
}

int main(int argc, char *argv[])
{

    if ((argc < 6) || (0 == atoi(argv[3])))
    {
        prusage();
    }
    const char *send_str = argv[1];
    const char *ip_str = argv[2];
    const char *port_str = argv[3];
    int perf_interval = stoi(argv[4]);
    int duration = stoi(argv[5]);
    Options::Parse(argc, argv);

    bool should_send = !strcmp(send_str, "send");

    signal(SIGINT, intHandler);

    // use this function to initialize the UDT library
    UDT::startup();

    struct addrinfo hints, *local, *peer;

    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (0 != getaddrinfo(NULL, "9000", &hints, &local))
    {
        cerr << "invalid network address.\n"
             << endl;
        return 0;
    }

    client = UDT::socket(local->ai_family, local->ai_socktype, local->ai_protocol);

    linger opt;
    opt.l_onoff = 1;
    opt.l_linger = 1;

    UDT::setsockopt(client, 0, UDT_LINGER, &opt, sizeof(linger)); 

    int timeout = 1000;
    UDT::setsockopt(client, 0, UDT_SNDTIMEO, &timeout, sizeof(int)); 

    freeaddrinfo(local);

    if (0 != getaddrinfo(ip_str, port_str, &hints, &peer))
    {
        cerr << "incorrect server/peer address. " << ip_str << ":" << port_str << endl;
        return 0;
    }

    if (UDT::ERROR == UDT::connect(client, peer->ai_addr, peer->ai_addrlen))
    {
        cerr << "connect: " << UDT::getlasterror().getErrorMessage() << endl;
        freeaddrinfo(peer);
        UDT::close(client);
        UDT::cleanup();
        return 0;
    }
    freeaddrinfo(peer);

    struct arg_struct args;
    args.s = &client;
    args.perf_interval = perf_interval;
    args.duration = duration;

    pthread_create(new pthread_t, NULL, monitor, (void *)&args);
    pthread_create(new pthread_t, NULL, send_ctr, (void *)&args);

    int batch_size = DATA_BATCH_SIZE;
    char *data = new char[batch_size];
    bzero(data, batch_size);

    while (!stop)
    {
        int cur_size = 0;
        int this_call_size = 0;
        while (!stop && cur_size < batch_size)
        {

            if (should_send)
            {
                this_call_size = UDT::send(client, data + cur_size, batch_size - cur_size, 0);
            }
            else
            {
                this_call_size = UDT::recv(client, data + cur_size, batch_size - cur_size, 0);
            }

            if (this_call_size == UDT::ERROR)
            {
                // cerr << "send/recv: " << UDT::getlasterror().getErrorMessage() << std::endl;
                break;
            }

            cur_size += this_call_size;
        }
    }

    

    UDT::close(client);

    delete[] data;

    // use this function to release the UDT library
    UDT::cleanup();

    return 0;
}

void *monitor(void *arguments)
{

    struct arg_struct *args = (arg_struct *)arguments;

    UDTSOCKET u = *(UDTSOCKET *)args->s;
    UDT::TRACEINFO perf;

    cout << "time,bandwidth,rtt,sent,lost,retr" << endl;
    int i = 0;
    while (true)
    {
        i++;
        if (UDT::ERROR == UDT::perfmon(u, &perf))
        {
            break;
        }
        else
        {
            cout << timestamp() << "," << perf.mbpsSendRate << "," << perf.msRTT << "," << perf.pktSent << "," << perf.pktSndLoss << "," << perf.pktRetrans << endl;
        }

        sleep(args->perf_interval);
    }
    return NULL;
}

void *send_ctr(void *arguments)
{

    struct arg_struct *args = (arg_struct *)arguments;
    ;

    UDTSOCKET ctru = *(UDTSOCKET *)args->s;
    UDT::TRACEINFO ctrperf;

    while (true)
    {
        if (UDT::ERROR == UDT::perfmon(ctru, &ctrperf))
        {
            break;
        }
        else
        {
            if (ctrperf.msTimeStamp > args->duration * 1000)
                stop = true;
        }

        sleep(1);
    }
    return NULL;
}
