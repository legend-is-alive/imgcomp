//----------------------------------------------------------------------------------
// Module for sending UDP notifcations to stepper program,
// for aming heater, fan, or cap shooter at kids and squirrels.
//----------------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/net.h>
#include <netdb.h>
#include <unistd.h>
#include <memory.h>
#include <sys/select.h>
#include <time.h>

#include "imgcomp.h"
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef int SOCKET;
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
typedef fd_set FD_SET;
typedef struct timeval TIMEVAL;
#define h_addr h_addr_list[0]


#define MAGIC_ID 0xf581     // Magic value to identify pings from this program.
#define MAGIC_PORTNUM 7777   // Magic port number for UDP pings (this is the port Joe picked for his app)
#define UDP_MAGIC 0x46c1

//-------------------------------------------------------------------------------------
// Structure for a test UDP packet.
typedef struct {
    short Ident;
    short Level; // How strong was the signal
    short xpos;  // 0-1000 X coordinate
    short ypos;  // 0-1000 Y coordinate
    short IsMotion; // Did it trigger saving an image
}Udp_t;

//-------------------------------------------------------------------------------------

#define STATUS_FAILED 0xFFFF

#define MAX_DESTS 5
static struct sockaddr_in dests[MAX_DESTS];
static int NumDests = 0;

static SOCKET sockUDP;

//--------------------------------------------------------------------------
// Form and send a UDP packet
//--------------------------------------------------------------------------
void SendUDP(int x, int y, int level, int motion)
{
    int wrote;
    int datasize;
    Udp_t Buf;

    if (NumDests == 0){
        fprintf(stderr, "UDP not initialized\n");
        return;
    }

    memset(&Buf, 0, sizeof(Buf));

    Buf.Ident = UDP_MAGIC;
    Buf.Level = level;
    Buf.xpos = x;
    Buf.ypos = y;
    Buf.IsMotion = motion;

    datasize = sizeof(Udp_t);

    for (int n=0;n<NumDests;n++){ // Loop through destinations -- can have targets for motion UDP
        wrote = sendto(sockUDP,(char *)&Buf, datasize, 0,(struct sockaddr*)&dests[n], sizeof(struct sockaddr_in));

        if (wrote == SOCKET_ERROR){
            perror("UDP sendto failed");
        }

        if (wrote < datasize ) {
            fprintf(stdout,"Wrote %d bytes of %d\n",wrote, datasize);
        }

        //printf("Sent UDP packet, x=%d\n",x);
    }
}

//--------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------
int InitUDP(char * HostNames)
{
    #ifdef _WIN32
        WSADATA wsaData;
    #endif

    struct hostent * hp;
    unsigned int addr=0;
    unsigned short PortNum = MAGIC_PORTNUM; // Use a different port, leave this for listener

    //-------------------------------------------------------------------
    // Resolve the remote address
    printf("Init UDP to %s\n",HostNames);

    memset(&dests,MAX_DESTS,sizeof(struct sockaddr_in));

    int s = 0;
    for (int a=0;;a++){
        char c = HostNames[a];
        if (c == ',' || c == '\0'){
            HostNames[a] = '\0';
            while (HostNames[s] == ' ' || HostNames[s] == '\t'){
                if (HostNames[s] == '\0'){
                    fprintf(stderr, "Empty hostname specified");
                    exit(-1);
                }
                s += 1;
            }

            printf("Hostname: '%s'\n",HostNames+s);
            if (NumDests >= MAX_DESTS){
                fprintf(stderr, "Maximum %d UDP destinations",MAX_DESTS);
                exit(-1);
            }

            hp = gethostbyname(HostNames+s);

            if (hp == NULL){
                // If address is not a host name, assume its a x.x.x.x format address.
                addr = inet_addr(HostNames+s);
                if (addr == INADDR_NONE) {
                    // Not a number format address either.  Give up.
                    fprintf(stderr,"Unable to resolve %s\n",HostNames);
                    exit(-1);
                }
                dests[NumDests].sin_addr.s_addr = addr;
                dests[NumDests].sin_family = AF_INET;
                dests[NumDests].sin_port = htons(PortNum);
                NumDests += 1;

            }else{
                // Resolved to a host name.
                memcpy(&(dests[NumDests].sin_addr),hp->h_addr,hp->h_length);
                dests[NumDests].sin_family = hp->h_addrtype;
                dests[NumDests].sin_port = htons(PortNum);
                NumDests += 1;
            }

            // Look for more host names in string
            s = a+1;
            if (c == '\0') break;
        }
    }

    //-------------------------------------------------------------------
    // Open socket (though we don't need to receive any packets, just send)
    {
        struct sockaddr_in local;

        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;

        // Use a different port so pan program can still use this port.
        local.sin_port = htons(PortNum+1);
        sockUDP = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sockUDP == INVALID_SOCKET){
            perror("couldn't create UDP socket");
            exit(-1);
        }

        if (bind(sockUDP,(struct sockaddr*)&local,sizeof(local) ) == SOCKET_ERROR) {
            perror("UDP socket open failed");
            exit(-1);
        }
    }

    return 0;
}

