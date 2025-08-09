#include "../../sdk/dexsdk.h"

/* Minimal user-space network diagnostic & configuration tool.
 * Commands:
 *   netdiag                      (one-shot show info + drain events)
 *   netdiag watch                (loop: show new events)
 *   netdiag pinggw [count]       (request additional gateway ICMP echo(s))
 *   netdiag setcfg ip gw mask    (update runtime network config)
 */

/* Mirror kernel structs/event codes (keep in sync with net.h) */
struct net_info {
	unsigned ip;
	unsigned gateway;
	unsigned netmask;
	unsigned char mac[6];
	unsigned char gateway_mac[6];
	unsigned flags;
};

struct net_event_rec {
	unsigned short type;
	unsigned short len;
	unsigned char data[64];
};

#define NET_EVENT_NONE            0
#define NET_EVENT_ARP_REQUEST     1
#define NET_EVENT_ARP_REPLY       2
#define NET_EVENT_ICMP_ECHO_REQ   3
#define NET_EVENT_ICMP_ECHO_REP   4
#define NET_EVENT_TCP_ESTABLISHED 5
#define NET_EVENT_TCP_DATA        6
#define NET_EVENT_TCP_CLOSED      7

static void ip4_print(unsigned ip){
	printf("%d.%d.%d.%d", (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF, ip&0xFF);
}

static void mac_print(unsigned char *m){
	int i; for(i=0;i<6;i++){ if(i) printf(":"); printf("%02x", m[i]); }
}

static const char *evt_name(unsigned t){
	switch(t){
		case NET_EVENT_ARP_REQUEST: return "ARP_REQUEST";
		case NET_EVENT_ARP_REPLY: return "ARP_REPLY";
		case NET_EVENT_ICMP_ECHO_REQ: return "ICMP_ECHO_REQ";
		case NET_EVENT_ICMP_ECHO_REP: return "ICMP_ECHO_REP";
		case NET_EVENT_TCP_ESTABLISHED: return "TCP_ESTABLISHED";
		case NET_EVENT_TCP_DATA: return "TCP_DATA";
		case NET_EVENT_TCP_CLOSED: return "TCP_CLOSED";
		default: return "?";
	}
}

static void show_info(){
	struct net_info ni; 
	if(dexsdk_systemcall(FXN_NET_INFO,(int)&ni,0,0,0,0)!=0){
		printf("netdiag: FXN_NET_INFO failed\n");
		return;
	}
	printf("IP: "); ip4_print(ni.ip); printf("\n");
	printf("Gateway: "); ip4_print(ni.gateway); printf("\n");
	printf("Netmask: "); ip4_print(ni.netmask); printf("\n");
	printf("MAC: "); mac_print(ni.mac); printf("\n");
	if(ni.flags & 1){ printf("Gateway MAC: "); mac_print(ni.gateway_mac); printf("\n"); }
	else printf("Gateway MAC: (pending)\n");
}

static int drain_events(int once){
	struct net_event_rec ev; int count=0; int r;
	while( (r=dexsdk_systemcall(FXN_NET_RECV,(int)&ev,0,0,0,0))>0 ){
		printf("[event %s len=%u] ", evt_name(ev.type), ev.len);
		if(ev.type==NET_EVENT_ARP_REQUEST && ev.len>=10){
			unsigned ip; ip = (ev.data[6]<<24)|(ev.data[7]<<16)|(ev.data[8]<<8)|ev.data[9];
			printf("sender="); mac_print(ev.data); printf(" spa="); ip4_print(ip); printf("\n");
		} else if((ev.type==NET_EVENT_ARP_REPLY) && ev.len>=6){
			printf("mac="); mac_print(ev.data); printf("\n");
		} else if(ev.type==NET_EVENT_ICMP_ECHO_REP && ev.len>=8){
			unsigned ident = (ev.data[4]<<8)|ev.data[5];
			unsigned seq = (ev.data[6]<<8)|ev.data[7];
			printf("ident=0x%04x seq=%u\n", ident, seq);
		} else if(ev.type==NET_EVENT_TCP_ESTABLISHED && ev.len==8){
			unsigned ip = (ev.data[0]<<24)|(ev.data[1]<<16)|(ev.data[2]<<8)|ev.data[3];
			unsigned sport = (ev.data[4]<<8)|ev.data[5];
			unsigned dport = (ev.data[6]<<8)|ev.data[7];
			printf("peer="); ip4_print(ip); printf(" sport=%u dport=%u\n", sport, dport);
		} else if(ev.type==NET_EVENT_TCP_DATA && ev.len>=8){
			unsigned paylen = (ev.data[0]<<24)|(ev.data[1]<<16)|(ev.data[2]<<8)|ev.data[3];
			unsigned ip = (ev.data[4]<<24)|(ev.data[5]<<16)|(ev.data[6]<<8)|ev.data[7];
			printf("peer="); ip4_print(ip); printf(" paylen=%u sample=", paylen);
			unsigned show = ev.len-8; unsigned i; if(show>16) show=16; for(i=0;i<show;i++){ unsigned char c=ev.data[8+i]; if(c>=32 && c<127) printf("%c", c); else printf("."); }
			printf("\n");
		} else if(ev.type==NET_EVENT_TCP_CLOSED && ev.len==8){
			unsigned ip = (ev.data[0]<<24)|(ev.data[1]<<16)|(ev.data[2]<<8)|ev.data[3];
			unsigned sport = (ev.data[4]<<8)|ev.data[5];
			unsigned dport = (ev.data[6]<<8)|ev.data[7];
			printf("peer="); ip4_print(ip); printf(" sport=%u dport=%u\n", sport, dport);
		} else {
			printf("rawlen=%u\n", ev.len);
		}
		count++;
		if(once) break;
	}
	return count;
}

static void usage(){
	printf("Usage: netdiag [watch|pinggw [count]|setcfg ip gw mask]\n");
	printf("  ip/gw/mask in dotted-decimal (e.g., 10.0.2.15)\n");
}

static unsigned parse_ip(const char *s){
	// Simple dotted quad parser (no sscanf dependency)
	unsigned parts[4]={0,0,0,0};
	int idx=0; unsigned val=0; const char *p=s; int digit=0;
	while(*p && idx<4){
		if(*p=='.'){
			parts[idx++]=val; val=0; digit=0; p++; continue;
		}
		if(*p<'0'||*p>'9') return 0;
		val = val*10 + (*p-'0'); if(val>255) return 0; digit=1; p++;
	}
	if(idx<3) parts[idx++]=val; // last segment
	if(idx!=4) return 0;
	return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
}

struct net_cfg_req { unsigned ip,gateway,netmask; };

int main(int argc, char **argv){
	int watch=0; int pinggw=0; int setcfg=0; unsigned pingcount=1;
	struct net_cfg_req cfg; int have_cfg=0;
	if(argc>1){
		if(strcmp(argv[1],"watch")==0) watch=1;
		else if(strcmp(argv[1],"pinggw")==0){
			pinggw=1; if(argc>2) pingcount=atoi(argv[2]);
		} else if(strcmp(argv[1],"setcfg")==0){
			if(argc<5){ usage(); return 0; }
			cfg.ip=parse_ip(argv[2]); cfg.gateway=parse_ip(argv[3]); cfg.netmask=parse_ip(argv[4]); have_cfg=1; setcfg=1;
		} else { usage(); return 0; }
	}
	if(setcfg && have_cfg){
		if(dexsdk_systemcall(FXN_NET_CFG,(int)&cfg,0,0,0,0)!=0){
			printf("setcfg failed\n");
		} else {
			printf("Configuration updated.\n");
		}
	}
	show_info();
	if(pinggw){
		printf("(pinggw) requesting %u gateway echo(s)\n", pingcount);
		dexsdk_systemcall(FXN_NET_SEND,1,pingcount,0,0,0); // op=1 count=N
	}
	drain_events(0);
	if(!watch) return 0;
	printf("-- watch mode (Ctrl-C to exit) --\n");
	while(1){
		int n = drain_events(0);
		if(n==0) dexsdk_systemcall(FXN_SLEEP,10,0,0,0,0);
	}
	return 0;
}

