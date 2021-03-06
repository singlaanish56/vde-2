/* Copyright 2005 Renzo Davoli - VDE-2
 * --pidfile/-p and cleanup management by Mattia Belletti (C) 2004.
 * Licensed under the GPLv2
 * Modified by Ludovico Gardenghi 2005
 * -g option (group management) by Daniel P. Berrange
 * dir permission patch by Alessio Caprari 2006
 */

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdint.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <net/if.h>
#include <stdarg.h>
#include <limits.h>
#include <grp.h>
#define _GNU_SOURCE
#include <getopt.h>

#include <config.h>
#include <vde.h>
#include <vdecommon.h>

#include "../vde_switch/switch.h"
#include "sockutils.h"
#include "consmgmt.h"

#include <af_ipn.h>

static struct swmodule swmi;
static unsigned int ctl_type;
static int mode = 0700;

static char real_ctl_socket[PATH_MAX];
static char *ctl_socket = real_ctl_socket;
static gid_t grp_owner = -1;

#define MODULENAME "kernel module interface"

static void handle_io(unsigned char type,int fd,int revents,void *arg)
{
	/*here OOB messages will be delivered for debug options */
}

static void cleanup(unsigned char type,int fd,void *unused)
{
	unlink(ctl_socket);
}

static struct option long_options[] = {
	{"sock", 1, 0, 's'},
	{"vdesock", 1, 0, 's'},
	{"unix", 1, 0, 's'},
	{"mod", 1, 0, 'm'},
	{"group", 1, 0, 'g'},
	{"tap", 1, 0, 't'},
	{"grab", 1, 0, 'G'},

};

#define Nlong_options (sizeof(long_options)/sizeof(struct option));

static void usage(void)
{
	printf(
			"(opts from datasock module)\n"
			"  -s, --sock SOCK            control directory pathname\n"
			"  -s, --vdesock SOCK         Same as --sock SOCK\n"
			"  -s, --unix SOCK            Same as --sock SOCK\n"
			"  -m, --mod MODE             Standard access mode for comm sockets (octal)\n"
			"  -g, --group GROUP          Group owner for comm sockets\n"
	    "  -t, --tap TAP           Enable routing through TAP tap interface\n"
	    "  -G, --grab INT          Enable routing grabbing an existing interface\n");
}

struct extinterface {
	char type;
	char *name;
	struct extinterface *next;
};

static struct extinterface *extifhead;
static struct extinterface **extiftail=&extifhead;

static void addextinterface(char type,char *name)
{
	struct extinterface *new=malloc(sizeof (struct extinterface));
	if (new) {
		new->type=type;
		new->name=strdup(name);
		new->next=NULL;
		*extiftail=new;
		extiftail=&(new->next);
	}
}

static void runextinterfaces(struct sockaddr_un *sun)
{
	struct extinterface *iface,*oldiface;
	struct ifreq ifr;
	for (iface=extifhead;iface != NULL;iface=oldiface)
	{
		int kvdefd;
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name,iface->name,IFNAMSIZ);
		if (iface->type == 't')
			ifr.ifr_flags=IPN_NODEFLAG_TAP;
		else
			ifr.ifr_flags=IPN_NODEFLAG_GRAB;
		//  printf("ioctl\n");
		kvdefd = socket(AF_IPN,SOCK_RAW,IPN_VDESWITCH);
		if (kvdefd < 0) {
			kvdefd = socket(AF_IPN_STOLEN,SOCK_RAW,IPN_VDESWITCH);
			if (kvdefd < 0) {
				printlog(LOG_ERR,"kvde_switch grab/tap error socket");
				exit(-1);
			}
		}
		if(bind(kvdefd, (struct sockaddr *) sun, sizeof(*sun)) < 0) {
			printlog(LOG_ERR,"cannot bind socket grab/tap");
			exit(-1);
		}
		if (ioctl(kvdefd, IPN_CONN_NETDEV, (void *) &ifr) < 0) {
			printlog(LOG_ERR, "%s interface %s error: %s", iface->name,
					(iface->type == 't')?"tap":"grab",strerror(errno));
			exit(-1);
		}
		free(iface->name);
		oldiface=iface->next;
		free(iface);
	}
	extifhead=NULL;
}

static int parseopt(int c, char *optarg)
{
	int outc=0;
	struct group *grp;
	switch (c) {
		case 's':
			/* This should returns NULL as the path probably does not exist */
			vde_realpath(optarg, ctl_socket);
			break;
		case 'm':
			sscanf(optarg,"%o",&mode);
			break;
		case 'g':
			if (!(grp = getgrnam(optarg))) {
				printlog(LOG_ERR, "No such group '%s'", optarg);
				exit(1);
			}
			grp_owner=grp->gr_gid;
			break;
		case 't':
		case 'G':
			addextinterface(c,optarg);
			break;
		default:
			outc=c;
	}
	return outc;
}

int check_kernel_support(void) {

	int kvdefd = socket(AF_IPN,SOCK_RAW,IPN_VDESWITCH);
	if (kvdefd < 0) {
		kvdefd = socket(AF_IPN_STOLEN,SOCK_RAW,IPN_VDESWITCH);
		if (kvdefd < 0) {
			printlog(LOG_ERR,"kvde_switch requires ipn and kvde_switch kernel modules loaded");
			return(-1);
		}
	}
	close(kvdefd);
	return 0;
}

static void init(void)
{
	int kvdefd;
	struct sockaddr_un sun;
	int family = AF_IPN;
	kvdefd = socket(AF_IPN,SOCK_RAW,IPN_VDESWITCH);
	if (kvdefd < 0) {
		family=AF_IPN_STOLEN;
		kvdefd = socket(AF_IPN_STOLEN,SOCK_RAW,IPN_VDESWITCH);
		if (kvdefd < 0) {
			printlog(LOG_ERR,"kvde_switch requires ipn and kvde_switch kernel modules loaded");
			exit(-1);
		}
	}
	sun.sun_family = family;
	snprintf(sun.sun_path,sizeof(sun.sun_path),"%s",ctl_socket);
	if(bind(kvdefd, (struct sockaddr *) &sun, sizeof(sun)) < 0) {
		printlog(LOG_ERR,"cannot bind socket %s",ctl_socket);
		exit(-1);
	}
	if(chmod(ctl_socket, mode) <0) {
		printlog(LOG_ERR, "chmod: %s", strerror(errno));
		exit(1);
	}
	if(chown(ctl_socket,-1,grp_owner) < 0) {
		printlog(LOG_ERR, "chown: %s", strerror(errno));
		exit(1);
	}
	runextinterfaces(&sun);
	add_fd(kvdefd,ctl_type,NULL);
}

static int showinfo(FILE *fd)
{
	printoutc(fd,"ctl dir %s",ctl_socket);
	printoutc(fd,"std mode 0%03o",mode);
	return 0;
}

static struct comlist cl[]={
	{"ds","============","DATA SOCKET MENU",NULL,NOARG},
	{"ds/showinfo","","show ds info",showinfo,NOARG|WITHFILE},
};

/*
static void delep (int fd, void* data, void *descr)
{
	if (fd>=0) remove_fd(fd);
	if (data) free(data);
	if (descr) free(descr);
}
*/

void start_datasock(void)
{
	strcpy(ctl_socket,(geteuid()==0)?VDESTDSOCK:VDETMPSOCK);
	swmi.swmnopts=Nlong_options;
	swmi.swmopts=long_options;
	swmi.usage=usage;
	swmi.parseopt=parseopt;
	swmi.init=init;
	swmi.handle_io=handle_io;
	swmi.cleanup=cleanup;
	ADDCL(cl);
	add_swm(&swmi);
}
