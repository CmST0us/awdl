#define _POSIX_C_SOURCE 200809L
#include "../hostap/backend.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../../components/awdl/control.h"

struct run {struct wa_backend *backend;struct Control *node;uint64_t start,cookie,active;unsigned freq,tx,rx,busy,acks;int error,peer;unsigned char local[6];};
static volatile sig_atomic_t stopped;
static void stop_signal(int n){(void)n;stopped=1;}
static uint64_t micros(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static int ip(char *const argv[]) {
 pid_t child=fork();if(child<0)return -errno;
 if(!child){execvp("ip",argv);_exit(127);}
 int status;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return -errno;}
 return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-EIO;
}
static void linklocal(const unsigned char *mac,char *out) {
 unsigned char addr[16]={0xfe,0x80};memcpy(addr+8,mac,3);addr[8]^=2;addr[11]=0xff;addr[12]=0xfe;memcpy(addr+13,mac+3,3);
 inet_ntop(AF_INET6,addr,out,INET6_ADDRSTRLEN);
}
static int emit(void *context,uint32_t kind,const unsigned char *data,size_t len) {
 struct run *r=context;int rc=0;
 if(kind==0) {
  if(r->active){r->busy++;return -EBUSY;}
  uint64_t cookie=++r->cookie;rc=wa_backend_command(r->backend,0x102,cookie,0,data,len);
  if(!rc){r->active=cookie;r->tx++;}else if(rc==-EBUSY){r->busy++;return rc;}
 } else if(kind==1) {
  char local[INET6_ADDRSTRLEN+4],peer[INET6_ADDRSTRLEN],mac[18];
  if(len!=34){rc=-EINVAL;goto done;}
  rc=wa_backend_command(r->backend,0x103,0,0,data,6);if(rc)goto done;
  rc=wa_backend_command(r->backend,0x105,0,0,data+6,28);if(rc)goto done;
  linklocal(r->local,local);strcat(local,"/64");linklocal(data,peer);
  snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",data[0],data[1],data[2],data[3],data[4],data[5]);
  rc=ip((char*[]){"ip","-6","addr","replace",local,"dev","awdl0","nodad",NULL});if(rc)goto done;
  rc=ip((char*[]){"ip","-6","neigh","replace",peer,"lladdr",mac,"nud","permanent","dev","awdl0",NULL});if(rc)goto done;
  rc=ip((char*[]){"ip","link","set","awdl0","up",NULL});if(rc)goto done;
  r->peer=1;printf("{\"event\":\"netdev-ready\",\"interface\":\"awdl0\",\"local\":\"%s\",\"peer\":\"%s\",\"mac\":\"%s\"}\n",local,peer,mac);
 } else if(kind==5) {
  uint64_t relative[2],absolute[2];memcpy(relative,data,16);
  for(int i=0;i<2;i++)absolute[i]=relative[i]?(r->start+relative[i]-1)*1000:0;
  rc=wa_backend_command(r->backend,0x106,0,0,absolute,16);
 } else if(kind==6) {
  if(len!=26){rc=-EINVAL;goto done;}
  uint8_t plan[26];int64_t relative;uint64_t absolute;memcpy(plan,data,26);memcpy(&relative,data,8);
  absolute=((int64_t)r->start+relative-1)*1000;memcpy(plan,&absolute,8);
  rc=wa_backend_command(r->backend,0x107,0,0,plan,26);
 } else if(kind==7) {
  if(len!=28){rc=-EINVAL;goto done;}
  rc=wa_backend_command(r->backend,0x108,0,0,data,len);
 } else if(kind==4){rc=-ENOLINK;}
done:
 if(rc){r->error=rc;stopped=1;}return rc;
}
static void event(void *context,const struct wa_event *e) {
 struct run *r=context;
 if(e->type==WA_NAN_EVENT_RX&&e->frame_len){r->rx++;control_rx(r->node,e->frame,e->frame_len,micros()-r->start+1);}
 else if(e->type==WA_NAN_EVENT_TX_DONE&&e->cookie==r->active){if(e->acked==WA_NAN_ACK_YES)r->acks++;r->active=0;}
}
int main(int argc,char **argv) {
 if(argc!=3&&argc!=5){fprintf(stderr,"usage: %s wlan-interface channel(149) peer-name local-name | stop\n",argv[0]);return 2;}
 int stopping=!strcmp(argv[2],"stop");
 if(!stopping&&argc!=5)return 2;
 int ch=atoi(argv[2]);if(!stopping&&ch!=149)return 2;
 struct run r={.freq=ch==6?2437:5000+5*ch};struct wa_caps caps;
 setvbuf(stdout,NULL,_IOLBF,0);signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);
 r.backend=wa_backend_open(argv[1],event,&r);if(!r.backend){perror("backend");return 1;}
 int rc;
 if(stopping){rc=wa_backend_command(r.backend,0x101,0,0,NULL,0);goto close;}
 rc=wa_backend_caps(r.backend,&caps);if(rc)goto close;
 printf("AWDL_LOCAL_HE supported=%u cap=",caps.he_supported);
 for(size_t i=0;i<sizeof(caps.he_cap);i++)printf("%02x",caps.he_cap[i]);
 putchar('\n');
 memcpy(r.local,caps.nmi,6);rc=wa_backend_command(r.backend,0x100,0,r.freq,NULL,0);if(rc)goto close;
 r.start=micros();printf("AWDL_CLOCK monotonic_us=%llu\n",(unsigned long long)r.start);r.node=control_new(caps.nmi,ch,161,argv[3],argv[4],caps.he_supported?caps.he_cap:NULL,3072,emit,&r);
 printf("{\"event\":\"started\"}\n");
 while(!stopped){
  uint64_t now=micros();
  uint64_t next=control_tick(r.node,now-r.start+1);if(stopped)break;
  uint64_t wake=r.start+next-1;
  now=micros();int ms=wake>now?(int)((wake-now+999)/1000):0;
  struct pollfd p[2]={{.fd=wa_backend_fd(r.backend),.events=POLLIN},{.fd=STDIN_FILENO,.events=POLLIN}};int got=poll(p,2,ms);
  if(got<0){if(errno==EINTR)continue;rc=-errno;break;}
  if(got&&(p[0].revents&(POLLERR|POLLHUP|POLLNVAL))){rc=-EIO;break;}
  if(got&&(p[1].revents&(POLLIN|POLLHUP))){
   char input[128];if(!fgets(input,sizeof(input),stdin)){stopped=1;continue;}
   input[strcspn(input,"\r\n")]=0;
   if(!strcmp(input,"STOP")){stopped=1;continue;}
   if(!strncmp(input,"SELECT ",7)){rc=control_select(r.node,input+7);if(rc){r.error=rc;stopped=1;}}
  }
  if(got&&(p[0].revents&POLLIN)){rc=wa_backend_receive(r.backend);if(rc&&rc!=-EAGAIN)break;rc=0;}
 }
 if(r.error)rc=r.error;
 int cleanup=wa_backend_command(r.backend,0x101,0,0,NULL,0);
 printf("{\"event\":\"stopped\",\"rc\":%d,\"cleanup\":%d,\"control_rx\":%u,\"control_tx\":%u,\"busy_drop\":%u}\n",rc,cleanup,r.rx,r.tx,r.busy);
 control_free(r.node);if(cleanup)rc=cleanup;
close:wa_backend_close(r.backend);return rc?1:0;
}
