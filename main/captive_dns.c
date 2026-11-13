#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "app.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Minimal, bounded DNS A responder for the setup AP. Unknown names resolve to
// 192.168.4.1 so Android/iOS connectivity probes reach our setup page.
void captive_dns_task(void *arg) {
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd<0) { vTaskDelete(NULL); return; }
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(53),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(fd,(struct sockaddr *)&local,sizeof(local))<0) { close(fd); vTaskDelete(NULL); return; }
    struct timeval timeout={.tv_sec=1};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    uint8_t b[512];
    for(;;) {
        struct sockaddr_in peer;
        socklen_t plen=sizeof(peer);
        int n=recvfrom(fd,b,sizeof(b)-16,0,(struct sockaddr *)&peer,&plen);
        if(n<17 || !wifi_provisioning() || (b[2]&0x80) || b[4]!=0 || b[5]!=1) continue;
        int end=12;
        while(end<n && b[end]) {
            int len=b[end];
            if(len>63 || end+1+len>=n) { end=n; break; }
            end+=len+1;
        }
        if(end+5>n) continue;
        end++;
        bool a=b[end]==0 && b[end+1]==1 && b[end+2]==0 && b[end+3]==1;
        end+=4;
        b[2]=0x81; b[3]=0x80; b[6]=0; b[7]=a?1:0;
        b[8]=b[9]=b[10]=b[11]=0;
        if(a) {
            const uint8_t answer[]={0xc0,0x0c,0,1,0,1,0,0,0,0,0,4,192,168,4,1};
            memcpy(b+end,answer,sizeof(answer)); end+=sizeof(answer);
        }
        sendto(fd,b,end,0,(struct sockaddr *)&peer,plen);
    }
}
