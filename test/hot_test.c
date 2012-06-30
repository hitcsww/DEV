#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <ctype.h>  
#include <sys/un.h>  
#include <sys/ioctl.h>  
#include <sys/socket.h>  
#include <linux/types.h>  
#include <linux/netlink.h>  
#include <errno.h>  
#include <unistd.h>  
#include <arpa/inet.h>  
#include <netinet/in.h>  

#define UEVENT_BUFFER_SIZE 2048  
#define joy_in_msg	"add@/devices/pci0000:00/0000:00:11.0/0000:02:00.0/usb2/2-2/2-2.1"
#define joy_out_msg	"remove@/devices/pci0000:00/0000:00:11.0/0000:02:00.0/usb2/2-2/2-2.1"


static int init_hotplug_sock() {
    const int buffersize = 1024;
    int ret;

    struct sockaddr_nl snl;
    bzero(&snl, sizeof (struct sockaddr_nl));
    snl.nl_family = AF_NETLINK;
    snl.nl_pid = getpid();
    snl.nl_groups = 1;

    int s = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (s == -1) {
        perror("socket");
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buffersize, sizeof (buffersize));

    ret = bind(s, (struct sockaddr *) &snl, sizeof (struct sockaddr_nl));
    if (ret < 0) {
        perror("bind");
        close(s);
        return -1;
    }

    return s;
}

int compare(char *str, char *ary){
	int i,j,k;
	i = strlen(str);
	j = strlen(ary);
	if(i!=j)	return 0;
	for(k=0; k<i; k++)
		if(str[k]!=ary[k])	return 0;
	return 1;
}
int main(int argc, char* argv[]) {
    int hotplug_sock = init_hotplug_sock();

    while (1) {
        /* Netlink message buffer */
        char buf[UEVENT_BUFFER_SIZE * 2] = {0};
        recv(hotplug_sock, &buf, sizeof (buf), 0);
        if (compare(buf,joy_in_msg)){
            system("insmod joydev.ko");
	    system("insmod js.ko");
	}
	if(compare(buf,joy_out_msg)){
	    system("rmmod joydev");
	    system("rmmod js");
	}
    }
    return 0;
}
