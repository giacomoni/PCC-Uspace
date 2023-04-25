#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <iostream>
#include "../core/udt.h"
#include "../core/options.h"

using namespace std;

void *senddata(void *);
void *recvdata(void *);
void *recv_monitor(void *s);
void *send_monitor(void *s);

bool stop = false;

struct arg_struct
{
   void *usocket;
   int perf_interval;
   int duration;
};

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

void prusage()
{
   cerr << "usage: appserver <send|recv> [server_port] perf_interval duration" << endl;
   exit(-1);
}

int main(int argc, char *argv[])
{

   if (strcmp(argv[1], "recv") && strcmp(argv[1], "send"))
   {
      prusage();
   }
   Options::Parse(argc, argv);

   bool should_recv = !strcmp(argv[1], "recv");

   UDT::startup();

   addrinfo hints;
   addrinfo *res;

   memset(&hints, 0, sizeof(struct addrinfo));

   hints.ai_flags = AI_PASSIVE;
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   string service(argv[2]);

   if (0 != getaddrinfo(NULL, service.c_str(), &hints, &res))
   {
      cerr << "illegal port number or port is busy.\n"
           << endl;
      return 0;
   }

   UDTSOCKET serv = UDT::socket(res->ai_family, res->ai_socktype, res->ai_protocol);

   if (UDT::ERROR == UDT::bind(serv, res->ai_addr, res->ai_addrlen))
   {
      cerr << "bind: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }

   freeaddrinfo(res);

   int perf_interval = stoi(argv[3]);
   int duration = stoi(argv[4]);


   cout << "server is ready at port: " << service << endl;

   if (UDT::ERROR == UDT::listen(serv, 1))
   {
      cerr << "listen: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }

   sockaddr_storage clientaddr;
   int addrlen = sizeof(clientaddr);

   UDTSOCKET udt_socket;

   timeval tv;
   UDT::UDSET readfds;

   tv.tv_sec = 100;
   tv.tv_usec = 0;

   UD_ZERO(&readfds);
   UD_SET(serv, &readfds);

   int accepted_connections = 0;
   while (accepted_connections < 1)
   {
      int res = UDT::select(0, &readfds, NULL, NULL, &tv);

      if ((res != UDT::ERROR) && (UD_ISSET(serv, &readfds)))
      {
         if (UDT::INVALID_SOCK == (udt_socket = UDT::accept(serv, (sockaddr *)&clientaddr, &addrlen)))
         {
            cerr << "accept: " << UDT::getlasterror().getErrorMessage() << endl;
            return 0;
         }

         char clienthost[NI_MAXHOST];
         char clientservice[NI_MAXSERV];
         getnameinfo((sockaddr *)&clientaddr, addrlen, clienthost, sizeof(clienthost), clientservice, sizeof(clientservice), NI_NUMERICHOST | NI_NUMERICSERV);
         cout << "new connection: " << clienthost << ":" << clientservice << endl;

         struct arg_struct args;
         args.usocket = new UDTSOCKET(udt_socket);
         args.perf_interval = perf_interval;
         args.duration = duration;


         pthread_t worker_thread;
         if (should_recv)
         {
            pthread_create(&worker_thread, NULL, recvdata, (void *)&args);
         }
         else
         {
            pthread_create(&worker_thread, NULL, senddata, (void *)&args);
         }

         pthread_join(worker_thread, NULL);
      }
      else
      {
         cout << "No connection established within 100s.... Terminating" << endl;
      }
      accepted_connections++;
   }

   UDT::close(serv);
   UDT::cleanup();

   return 0;
}

void *senddata(void *arguments)
{
   struct arg_struct *args = (arg_struct *)arguments;

   UDTSOCKET sender = *(UDTSOCKET *)args->usocket;
   delete (UDTSOCKET *)args->usocket;
   pthread_create(new pthread_t, NULL, send_monitor, &sender);
   char *data;
   int size = 100000000;
   data = new char[size];

   while (true)
   {
      int ssize = 0;
      int ss;
      while (ssize < size)
      {
         if (UDT::ERROR == (ss = UDT::send(sender, data + ssize, size - ssize, 0)))
         {
            cout << "send:" << UDT::getlasterror().getErrorMessage() << endl;
            break;
         }
         ssize += ss;
      }

      if (ssize < size)
         break;
   }

   delete[] data;

   UDT::close(sender);

   return NULL;
}

void *recvdata(void *arguments)
{
   struct arg_struct *args = (arg_struct *)arguments;

   UDTSOCKET recver = *(UDTSOCKET *)args->usocket;
   delete (UDTSOCKET *)args->usocket;

   int timeout = (args->duration+2)*1000;
   UDT::setsockopt(recver, 0, UDT_RCVTIMEO, &timeout, sizeof(int)); 

   struct arg_struct args2;
   args2.usocket = &recver;
   args2.perf_interval = args->perf_interval;
   args2.duration = args->duration;

   pthread_t worker_thread;
   pthread_create(&worker_thread, NULL, recv_monitor, (void *)&args2);

   char *data;
   int size = 100000000; // 100MB
   data = new char[size];

   while (!stop)
   {
      int rsize = 0;
      int rs;
      while (!stop && rsize < size)
      {
         if (UDT::ERROR == (rs = UDT::recv(recver, data + rsize, size - rsize, 0)))
         {
            cerr << "recv:" << UDT::getlasterror().getErrorMessage() << endl;
            stop = true;
            break;
         }

         rsize += rs;
      }

      if (rsize < size)
      { // If bytes received are less than the ones expected, stop receiving
         stop = true;
         break;
      }
   }

   pthread_join(worker_thread, NULL);
   delete[] data;

   UDT::close(recver);
   
   return NULL;
}

void *recv_monitor(void *arguments)
{
   struct arg_struct *args2 = (arg_struct *)arguments;
   UDTSOCKET u2 = *(UDTSOCKET *)args2->usocket;
   int i = 0;

   UDT::TRACEINFO perf;

   cout << "time,bandwidth"
        << endl;

   while (!stop)
   {
      ++i;
      if (UDT::ERROR == UDT::perfmon(u2, &perf))
      {
         cerr << "perfmon: " << UDT::getlasterror().getErrorMessage() << endl;
         break;
      }
      else
      {
         cout << timestamp() << "," << perf.mbpsRecvRate << endl;
      }

      if(perf.msTimeStamp >= args2->duration * 1000)
         stop=true;

      sleep(args2->perf_interval);
   }


   return NULL;
}

void *send_monitor(void *s)
{
   UDTSOCKET u = *(UDTSOCKET *)s;
   int i = 0;

   UDT::TRACEINFO perf;

   cout << "Send Rate(Mb/s)\tRTT(ms)\t\tSent\t\tLost" << endl;

   while (true)
   {
      ++i;
      sleep(1);

      if (UDT::ERROR == UDT::perfmon(u, &perf))
      {
         cout << "perfmon: " << UDT::getlasterror().getErrorMessage() << endl;
         break;
      }
   }

   return NULL;
}
