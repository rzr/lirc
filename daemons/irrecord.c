/*      $Id: irrecord.c,v 5.6 1999/09/12 13:52:18 columbus Exp $      */

/****************************************************************************
 ** irrecord.c **************************************************************
 ****************************************************************************
 *
 * irrecord -  application for recording IR-codes for usage with lircd
 *
 * Copyright (C) 1998,99 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>

#include "drivers/lirc.h"

#include "hardware.h"
#include "dump_config.h"
#include "ir_remote.h"
#include "config_file.h"

void flushhw(void);
int resethw(void);
int waitfordata(unsigned long maxusec);
int availabledata(void);
void get_repeat_bit(struct ir_remote *remote,ir_code xor);
void get_pre_data(struct ir_remote *remote);
void get_post_data(struct ir_remote *remote);
int get_lengths(struct ir_remote *remote,int force);
struct lengths *new_length(lirc_t length);
int add_length(struct lengths **first,lirc_t length);
void free_lengths(struct lengths *first);
void merge_lengths(struct lengths *first);
void get_scheme(struct ir_remote *remote);
struct lengths *get_max_length(struct lengths *first,unsigned int *sump);
void unlink_length(struct lengths **first,struct lengths *remove);
int get_trail_length(struct ir_remote *remote);
int get_lead_length(struct ir_remote *remote);
int get_repeat_length(struct ir_remote *remote);
int get_header_length(struct ir_remote *remote);
int get_data_length(struct ir_remote *remote);
int get_gap_length(struct ir_remote *remote);
void fprint_copyright(FILE *fout);

extern struct hardware hw;
extern struct ir_remote *last_remote;

char *progname;

struct ir_remote remote;
struct ir_ncode ncode;

#define IRRECORD_VERSION "0.5"
#define BUTTON 80+1
#define RETRIES 10

#define min(a,b) (a>b ? b:a)
#define max(a,b) (a>b ? a:b)


/* the longest signal I've seen up to now was 48-bit signal with header */

#define MAX_SIGNALS 200

lirc_t signals[MAX_SIGNALS];

#define AEPS 100
#define EPS 30

/* some threshold values */

#define TH_SPACE_ENC   80	/* I want less than 20% mismatches */
#define TH_HEADER      90
#define TH_REPEAT      90
#define TH_TRAIL       90
#define TH_LEAD        90
#define TH_IS_BIT      15

#define MIN_GAP  20000
#define MAX_GAP 100000

#define SAMPLES 80

#ifdef DEBUG
int debug=10;
FILE *lf=NULL;
char *hostname="k6";
int daemonized=0;

void logprintf(int level,char *format_str, ...)
{
	time_t current;
	char *currents;
	va_list ap;  
	
	if(level>debug) return;
	
	current=time(&current);
	currents=ctime(&current);
	
	if(lf) fprintf(lf,"%15.15s %s %s: ",currents+4,hostname,progname);
	if(!daemonized) fprintf(stderr,"%s: ",progname);
	va_start(ap,format_str);
	if(lf) {vfprintf(lf,format_str,ap);fflush(lf);}
	if(!daemonized) {vfprintf(stderr,format_str,ap);fflush(stderr);}
	va_end(ap);
}

void logperror(int level,const char *s)
{
	if(level>debug) return;

	if(s!=NULL)
	{
		logprintf(level,"%s: %s\n",s,strerror(errno));
	}
	else
	{
		logprintf(level,"%s\n",strerror(errno));
	}
}
#else
void logprintf(int level,char *format_str, ...) {}
void logperror(int level,const char *s) {}
#endif

int main(int argc,char **argv)
{
	char *filename;
	FILE *fout,*fin;
	int flags;
	int retval=EXIT_SUCCESS;
	ir_code pre,code,post;
	int repeat_flag;
	lirc_t remaining_gap;
	ir_code first;
	int flag,force;
	int retries;
	struct ir_remote *remotes;

	progname=argv[0];
	force=0;
	while(1)
	{
		int c;
		static struct option long_options[] =
		{
			{"help",no_argument,NULL,'h'},
			{"version",no_argument,NULL,'v'},
			{"force",no_argument,NULL,'f'},
			{0, 0, 0, 0}
		};
		c = getopt_long(argc,argv,"hvf",long_options,NULL);
		if(c==-1)
			break;
		switch (c)
		{
		case 'h':
			printf("Usage: %s [options] [file]\n",progname);
			printf("\t -h --help\t\tdisplay this message\n");
			printf("\t -v --version\t\tdisplay version\n");
			printf("\t -f --force\t\tforce raw mode\n");
			exit(EXIT_SUCCESS);
		case 'v':
			printf("irrecord %s\n",IRRECORD_VERSION);
			exit(EXIT_SUCCESS);
		case 'f':
			force=1;
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}
	if(optind+1!=argc)
	{
		fprintf(stderr,"%s: invalid argument count\n",progname);
		exit(EXIT_FAILURE);
	}
	filename=argv[optind];
	fin=fopen(filename,"r");
	if(fin!=NULL)
	{
		fclose(fin);
		fprintf(stderr,"%s: file \"%s\" already exists\n",progname,
			filename);
		exit(EXIT_FAILURE);
	}
	fout=fopen(filename,"w");
	if(fout==NULL)
	{
		fprintf(stderr,"%s: could not open file %s\n",progname,
			filename);
		perror(progname);
		exit(EXIT_FAILURE);
	}
	printf("\nirrecord -  application for recording IR-codes"
	       " for usage with lirc\n"
	       "\n"  
	       "Copyright (C) 1998,1999 Christoph Bartelmus"
	       "(lirc@bartelmus.de)\n");
	printf("\n");
	
	if(hw.init_func)
	{
		if(!hw.init_func())
		{
			fprintf(stderr,"%s: could not init hardware"
				" (lircd running ? --> close it)\n",progname);
			fclose(fout);
			unlink(filename);
			exit(EXIT_FAILURE);
		}
	}
	
	if(hw.rec_mode==LIRC_MODE_STRING)
	{
		fprintf(stderr,"%s: no config file necessary\n",progname);
		fclose(fout);
		unlink(filename);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_SUCCESS);
	}
	if(hw.rec_mode!=LIRC_MODE_MODE2 &&
	   hw.rec_mode!=LIRC_MODE_CODE &&
	   hw.rec_mode!=LIRC_MODE_LIRCCODE)
	{
		fprintf(stderr,"%s: mode not supported\n",progname);
		fclose(fout);
		unlink(filename);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	
	flags=fcntl(hw.fd,F_GETFL,0);
	if(flags==-1 || fcntl(hw.fd,F_SETFL,flags|O_NONBLOCK)==-1)
	{
		fprintf(stderr,"%s: could not set O_NONBLOCK flag\n",
			progname);
		fclose(fout);
		unlink(filename);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	
	printf(
"This program will record the signals from your remote control\n"
"and create a config file for lircd.\n\n"
"A proper config file for lircd is maybe the most vital part of this\n"
"package, so you should invest some time to create a working config file.\n"
"Although I put a good deal of effort in this program it is often\n"
"not possible to automatically recognize all features of a remote control.\n"
"Often short-comings of the receiver hardware make it nearly impossible.\n"
"If the config file this program generated does not work like expected\n"
"read the documentation of this package how to get help.\n"
"\n"
"IMPORTANT: The license of the config files created by this program requires\n"
"that you send them to the author. If you don't like this license exit this\n"
"program now! Otherwise press return.\n\n");
	
	if(getchar()!='\n')
	{
		fclose(fout);
		unlink(filename);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}

	remote.name=filename;
	switch(hw.rec_mode)
	{
	case LIRC_MODE_MODE2:
		if(!get_lengths(&remote,force))
		{
			if(remote.gap==0)
			{
				fprintf(stderr,"%s: gap not found,"
					" can�t continue\n",progname);
				fclose(fout);
				unlink(filename);
				if(hw.deinit_func) hw.deinit_func();
				exit(EXIT_FAILURE);
			}
			printf("Creating config file in raw mode.\n");
			remote.flags&=~(SPACE_ENC|SHIFT_ENC);
			remote.flags|=RAW_CODES;
			remote.eps=EPS;
			remote.aeps=AEPS;
			break;
		}
		
#               ifdef DEBUG
		printf("%d %lu %lu %lu %lu %lu %d %d %d %lu\n",
		       remote.bits,
		       (unsigned long) remote.pone,
		       (unsigned long) remote.sone,
		       (unsigned long) remote.pzero,
		       (unsigned long) remote.szero,
		       (unsigned long) remote.ptrail,
		       remote.flags,remote.eps,remote.aeps,
		       (unsigned long) remote.gap);
#               endif
		break;
	case LIRC_MODE_CODE:
	case LIRC_MODE_LIRCCODE:
		if(hw.rec_mode==LIRC_MODE_CODE) remote.bits=CHAR_BIT;
		else remote.bits=hw.code_length;
		if(!get_gap_length(&remote))
		{
			fprintf(stderr,"%s: gap not found,"
				" can�t continue\n",progname);
			fclose(fout);
			unlink(filename);
			if(hw.deinit_func) hw.deinit_func();
			exit(EXIT_FAILURE);
		}
		break;
	}
	
	printf("Now enter the names for the buttons.\n");

	fprint_copyright(fout);
	fprint_comment(fout,&remote);
	fprint_remote_head(fout,&remote);
	fprint_remote_signal_head(fout,&remote);
	while(1)
	{
		char buffer[BUTTON];
		char *string;

		printf("\nPlease enter the name for the next button\n");
		string=fgets(buffer,BUTTON,stdin);
		
		if(string!=buffer)
		{
			fprintf(stderr,"%s: fgets() failed\n",progname);
			retval=EXIT_FAILURE;
			break;
		}
		buffer[strlen(buffer)-1]=0;
		if(strchr(buffer,' ') || strchr(buffer,'\t'))
		{
			printf("The name must not contain any whitespace.\n");
			printf("Please try again.\n");
			continue;
		}
		if(strcasecmp(buffer,"begin")==0 
		   || strcasecmp(buffer,"end")==0)
		{
			printf("'%s' is not allowed as button name\n",buffer);
			printf("Please try again.\n");
			continue;
		}
		if(strlen(buffer)==0)
		{
			break;
		}
		
		if(remote.flags&RAW_CODES)
		{
			flushhw();
		}
		else
		{
			while(availabledata())
			{
				hw.rec_func(NULL);
			}
		}
		printf("\nNow press button \"%s\".\n",buffer);fflush(stdout);
		
		if(remote.flags&RAW_CODES)
		{
			lirc_t data,sum;
			unsigned int count;
			int ret;

			count=0;sum=0;
			while(count<MAX_SIGNALS)
			{
				unsigned long timeout;
				
				if(count==0) timeout=10000000;
				else timeout=remote.gap*5;
				if(!waitfordata(timeout))
				{
					if(count==0)
					{
						fprintf(stderr,"%s: no data for 10 secs,"
							" aborting\n",progname);
						retval=EXIT_FAILURE;
						break;
					}
					data=remote.gap;
				}
				else
				{
					ret=read(hw.fd,&data,sizeof(data));
					if(ret!=sizeof(unsigned long))
					{
						fprintf(stderr,"%s: read() failed\n",
							progname);
						perror(progname);
						retval=EXIT_FAILURE;
						break;
					}
				}
				if(count==0)
				{
					if(!is_space(data) ||
					   data<remote.gap-remote.gap*remote.eps/100)
					{
						printf("Sorry, something "
						       "went wrong.\n");
						sleep(3);
						printf("Try again.\n");
						flushhw();
						count=0;
						continue;
					}
				}
				else
				{
					if(is_space(data) && 
					   (is_const(&remote) ? 
					    data>(remote.gap>sum ? (remote.gap-sum)*(100-remote.eps)/100:0)
					    :
					    data>remote.gap*(100-remote.eps)/100))
					{
						printf("Got it.\n");
						printf("Signal length is %d\n",
						       count-1);
						if(count%2)
						{
							printf("That's weird because "
							       "the signal length "
							       "must be odd!\n");
							sleep(3);
							printf("Try again.\n");
							flushhw();
							count=0;
							continue;
						}
						else
						{
							ncode.name=buffer;
							ncode.length=count-1;
							ncode.signals=signals;
							fprint_remote_signal(fout,
									     &remote,
									     &ncode);
							break;
						}
					}
					signals[count-1]=data&PULSE_MASK;
					sum+=data&PULSE_MASK;
				}
				count++;
			}
			if(count==MAX_SIGNALS)
			{
				printf("Signal is too long.\n");
			}
			if(retval==EXIT_FAILURE) break;
			continue;
		}
		retries=RETRIES;
		while(retries>0)
		{
			int flag;

			if(!waitfordata(10000000))
			{
				fprintf(stderr,"%s: no data for 10 secs,"
					" aborting\n",progname);
				retval=EXIT_FAILURE;
				break;
			}
			last_remote=NULL;
			flag=0;
			sleep(1);
			while(availabledata())
			{
				hw.rec_func(NULL);
				if(hw.decode_func(&remote,&pre,&code,&post,
						  &repeat_flag,&remaining_gap))
				{
					flag=1;
					break;
				}
			}
			if(flag)
			{
				ncode.name=buffer;
				ncode.code=code;
				fprint_remote_signal(fout,&remote,&ncode);
				break;
			}
			else
			{
				printf("Something went wrong. ");
				if(retries>1)
				{
					fflush(stdout);sleep(3);
					if(!resethw())
					{
						fprintf(stderr,"%s: Could not reset "
							"hardware.\n",progname);
						retval=EXIT_FAILURE;
						break;
					}
					flushhw();
					printf("Press button again. "
					       "(%d retries left)\n",retries-1);
				}
				else
				{
					printf("\n");
					printf("Try using the -f option.\n");
				}
				retries--;
				continue;
			}
		}
		if(retries==0) retval=EXIT_FAILURE;
		if(retval==EXIT_FAILURE) break;
	}
	fprint_remote_signal_foot(fout,&remote);
	fprint_remote_foot(fout,&remote);
	fclose(fout);

	if(retval==EXIT_FAILURE)
	{
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	
	if(remote.flags&RAW_CODES)
	{
		return(EXIT_SUCCESS);
	}
	if(!resethw())
	{
		fprintf(stderr,"%s: Could not reset hardware.\n",progname);
		exit(EXIT_FAILURE);
	}
	
	fin=fopen(filename,"r");
	if(fin==NULL)
	{
		fprintf(stderr,"%s: could not reopen config file\n",progname);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	remotes=read_config(fin);
	fclose(fin);
	if(remotes==NULL)
	{
		fprintf(stderr,"%s: config file contains no valid "
			"remote control definition\n",progname);
		fprintf(stderr,"%s: this shouldn't ever happen!\n",progname);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	if(remotes==(void *) -1)
	{
		fprintf(stderr,"%s: reading of config file failed\n",
			progname);
		fprintf(stderr,"%s: this shouldn't ever happen!\n",progname);
		if(hw.deinit_func) hw.deinit_func();
		exit(EXIT_FAILURE);
	}
	
	printf("Checking for repeat bit.\n");
	printf("Please press an arbitrary button repeatedly (don't hold it down).\n");
	retries=30;flag=0;first=0;
	while(retval==EXIT_SUCCESS && retries>0)
	{
		while(availabledata())
		{
			hw.rec_func(NULL);
		}
		if(!waitfordata(10000000))
		{
			printf("%s: no data for 10 secs, aborting\n",
			       progname);
			retval=EXIT_FAILURE;
			break;
		}
		hw.rec_func(NULL);
		if(hw.decode_func(remotes,&pre,&code,&post,
				  &repeat_flag,&remaining_gap))
		{
			if(flag==0)
			{
				flag=1;
				first=code;
			}
			else if(!repeat_flag)
			{
				if(first^code)
				{
					get_repeat_bit(remotes,first^code);
					if(remotes->repeat_bit>0)
						printf("\nRepeat bit is %d.\n",
						       remotes->repeat_bit);
					else
						printf("\nInvalid repeat bit.\n");
					break;
				}
				printf(".");fflush(stdout);
				retries--;
			}
		}
		else
		{
			printf(".");fflush(stdout);
			retries--;
		}
		if(retries==0)
		{
			printf("\nNo repeat bit found.\n");
		}
	}
	if(hw.deinit_func) hw.deinit_func();
	get_pre_data(remotes);
	get_post_data(remotes);
	
	/* write final config file */
	fout=fopen(filename,"w");
	if(fout==NULL)
	{
		fprintf(stderr,"%s: could not open file \"%s\"\n",progname,
			filename);
		perror(progname);
		free_config(remotes);
		return(EXIT_FAILURE);
	}
	fprint_copyright(fout);
	fprint_remotes(fout,remotes);
	free_config(remotes);
	return(EXIT_SUCCESS);
}

void flushhw()
{
	size_t size=1;
	char buffer[sizeof(ir_code)];

	switch(hw.rec_mode)
	{
	case LIRC_MODE_MODE2:
		size=sizeof(lirc_t);
		break;
	case LIRC_MODE_CODE:
		size=sizeof(unsigned char);
		break;
	case LIRC_MODE_LIRCCODE:
		size=hw.code_length/CHAR_BIT;
		if(hw.code_length%CHAR_BIT) size++;
		break;
	}
	while(read(hw.fd,buffer,size)==size);
}

int resethw()
{
	int flags;

	if(hw.deinit_func) hw.deinit_func();
	if(hw.init_func)
	{
		if(!hw.init_func())
			return(0);
	}
	flags=fcntl(hw.fd,F_GETFL,0);
	if(flags==-1 || fcntl(hw.fd,F_SETFL,flags|O_NONBLOCK)==-1)
	{
		if(hw.deinit_func) hw.deinit_func();
		return(0);
	}
	return(1);
}

int waitfordata(unsigned long maxusec)
{
	fd_set fds;
	int ret;
	struct timeval tv;

	while(1)
	{
		FD_ZERO(&fds);
		FD_SET(hw.fd,&fds);
		do{
			do{
				if(maxusec>0)
				{
					tv.tv_sec=maxusec/1000000;
					tv.tv_usec=maxusec%1000000;
					ret=select(hw.fd+1,&fds,NULL,NULL,&tv);
					if(ret==0) return(0);
				}
				else
				{
					ret=select(hw.fd+1,&fds,NULL,NULL,NULL);
				}
			}
			while(ret==-1 && errno==EINTR);
			if(ret==-1)
			{
				logprintf(0,"select() failed\n");
				logperror(0,NULL);
				continue;
			}
		}
		while(ret==-1);
		
                if(FD_ISSET(hw.fd,&fds))
                {
                        /* we will read later */
			return(1);
                }	
	}
}

int availabledata(void)
{
	fd_set fds;
	int ret;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(hw.fd,&fds);
	do{
		do{
			tv.tv_sec=0;
			tv.tv_usec=0;
			ret=select(hw.fd+1,&fds,NULL,NULL,&tv);
		}
		while(ret==-1 && errno==EINTR);
		if(ret==-1)
		{
			logprintf(0,"select() failed\n");
			logperror(0,NULL);
			continue;
		}
	}
	while(ret==-1);
	
	if(FD_ISSET(hw.fd,&fds))
	{
		return(1);
	}	
	return(0);
}

void get_repeat_bit(struct ir_remote *remote,ir_code xor)
{
	ir_code mask;
	int repeat_bit;
	struct ir_ncode *codes;

	if(!remote->codes) return;


	mask=((ir_code) 1)<<(remote->bits+remote->pre_data_bits+remote->post_data_bits-1);
	repeat_bit=1;
	while(mask)
	{
		if(mask==xor) break;
		mask=mask>>1;
		repeat_bit++;
	}
	if(mask)
	{
		remote->repeat_bit=repeat_bit;
		
		codes=remote->codes;
		while(codes->name!=NULL)
		{
			codes->code&=~mask;
			codes++;
		}
	}
}

void get_pre_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask,last;
	int count,i;
	
	if(remote->bits==0) return;

	mask=(-1);
	codes=remote->codes;
	if(codes->name!=NULL)
	{
		last=codes->code;
		codes++;
	}
	if(codes->name==NULL) return; /* at least 2 codes needed */
	while(codes->name!=NULL)
	{
		mask&=~(last^codes->code);
		last=codes->code;
		codes++;
	}
	count=0;
#ifdef LONG_IR_CODE
	while(mask&0x8000000000000000LL)
#else
	while(mask&0x80000000L)
#endif
	{
		count++;
		mask=mask<<1;
	}
	count-=sizeof(ir_code)*CHAR_BIT-remote->bits;
	if(count>0)
	{
		mask=0;
		for(i=0;i<count;i++)
		{
			mask=mask<<1;
			mask|=1;
		}
		remote->bits-=count;
		mask=mask<<(remote->bits);
		remote->pre_data_bits=count;
		remote->pre_data=(last&mask)>>(remote->bits);

		codes=remote->codes;
		while(codes->name!=NULL)
		{
			codes->code&=~mask;
			codes++;
		}
	}
}

void get_post_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask,last;
	int count,i;
	
	if(remote->bits==0) return;

	mask=(-1);
	codes=remote->codes;
	if(codes->name!=NULL)
	{
		last=codes->code;
		codes++;
	}
	if(codes->name==NULL) return; /* at least 2 codes needed */
	while(codes->name!=NULL)
	{
		mask&=~(last^codes->code);
		last=codes->code;
		codes++;
	}
	count=0;
	while(mask&0x1)
	{
		count++;
		mask=mask>>1;
	}
	if(count>0)
	{
		mask=0;
		for(i=0;i<count;i++)
		{
			mask=mask<<1;
			mask|=1;
		}
		remote->bits-=count;
		remote->post_data_bits=count;
		remote->post_data=last&mask;

		codes=remote->codes;
		while(codes->name!=NULL)
		{
			codes->code=codes->code>>count;
			codes++;
		}
	}
}

/* analyse stuff */

struct lengths
{
	unsigned int count;
	lirc_t sum,upper_bound,lower_bound,min,max;
	struct lengths *next;
};

enum analyse_mode {MODE_GAP,MODE_HAVE_GAP};

struct lengths *first_space=NULL,*first_pulse=NULL;
struct lengths *first_sum=NULL,*first_gap=NULL,*first_repeat_gap=NULL;
struct lengths *first_signal_length=NULL;
struct lengths *first_headerp=NULL,*first_headers=NULL;
struct lengths *first_1lead=NULL,*first_3lead=NULL,*first_trail=NULL;
struct lengths *first_repeatp=NULL,*first_repeats=NULL;
unsigned long lengths[MAX_SIGNALS];
unsigned int count,count_spaces,count_3repeats,count_5repeats,count_signals;

int get_lengths(struct ir_remote *remote,int force)
{
	int ret,retval;
	lirc_t data,average,sum,remaining_gap;
	enum analyse_mode mode=MODE_GAP;

	printf("Now start pressing buttons on your remote control.\n\n");
	printf("It is very important that you press many different buttons and \n"
	       "hold them down for approximately one second.\n"
	       "Each button should generate at least one dot but in no case\n"
	       "more than ten dots of output.\n");
	flushhw();
	retval=1;
	average=0;sum=0;count=0;count_spaces=0;
	count_3repeats=0;count_5repeats=0;count_signals=0;
	while(1)
	{
		if(!waitfordata(10000000))
		{
			fprintf(stderr,"%s: no data for 10 secs, aborting\n",
				progname);
			retval=0;
			break;
		}
		ret=read(hw.fd,&data,sizeof(data));
		if(ret!=sizeof(data))
		{
			fprintf(stderr,"%s: read() failed\n",
				progname);
			perror(progname);
			retval=0;
			break;
		}
		count++;
		if(mode==MODE_GAP)
		{
			sum+=data&PULSE_MASK;
			if(average==0 && is_space(data))
			{
				if(data>100000)
				{
					sum=0;
					continue;
				}
				average=data;
			}
			else if(is_space(data))
			{
				if(data>MIN_GAP || data>100*average ||
				   /* this MUST be a gap */
				   (count_spaces>10 && data>5*average))  
					/* this should be a gap */
				{
					struct lengths *scan;
					int maxcount;
					static int lastmaxcount=0;
					int i;

					add_length(&first_sum,sum);
					merge_lengths(first_sum);
					add_length(&first_gap,data);
					merge_lengths(first_gap);
					sum=0;count_spaces=0;average=0;
					
					maxcount=0;
					scan=first_sum;
					while(scan)
					{
						maxcount=max(maxcount,
							     scan->count);
						if(scan->count>SAMPLES)
						{
							remote->gap=scan->sum/scan->count;
							remote->flags|=CONST_LENGTH;
							printf("\nFound const length: %lu\n",(unsigned long) remote->gap);
							break;
						}
						scan=scan->next;
					}
					if(scan==NULL)
					{
						scan=first_gap;
						while(scan)
						{
							maxcount=max(maxcount,
								     scan->count);
							if(scan->count>SAMPLES)
							{
								remote->gap=scan->sum/scan->count;
								printf("\nFound gap: %lu\n",(unsigned long) remote->gap);
								break;
							}
							scan=scan->next;
						}
					}
					if(scan!=NULL)
					{
						mode=MODE_HAVE_GAP;
						sum=0;
						count=0;
						remaining_gap=
						is_const(remote) ? 
						(remote->gap>data ? remote->gap-data:0):
						(has_repeat_gap(remote) ? remote->repeat_gap:remote->gap);
						if(force)
						{
							retval=0;
							break;
						}
						continue;
					}
					
					for(i=maxcount-lastmaxcount;i>0;i--)
					{
						printf(".");
						fflush(stdout);
					}
					lastmaxcount=maxcount;
					
					continue;
				}
				average=(average*count_spaces+data)
				/(count_spaces+1);
				count_spaces++;
			}
			if(count>SAMPLES*MAX_SIGNALS)
			{
				fprintf(stderr,"\n%s: could not find gap.\n",
					progname);
				retval=0;
				break;
			}
		}
		else if(mode==MODE_HAVE_GAP)
		{
			if(count<=MAX_SIGNALS)
			{
				signals[count-1]=data&PULSE_MASK;
			}
			else
			{
				fprintf(stderr,"%s: signal too long\n",
					progname);
				retval=0;
				break;
			}
			if(is_const(remote))
			{
				remaining_gap=remote->gap>sum ?
				remote->gap-sum:0;
			}
			else
			{
				remaining_gap=remote->gap;
			}
			sum+=data&PULSE_MASK;

			if((data&PULSE_MASK)>=remaining_gap*(100-EPS)/100
			   || (data&PULSE_MASK)>=remaining_gap-AEPS)
			{
				if(is_space(data))
				{
				        /* signal complete */
					if(count==4)
					{
						count_3repeats++;
						add_length(&first_repeatp,signals[0]);
						merge_lengths(first_repeatp);
						add_length(&first_repeats,signals[1]);
						merge_lengths(first_repeats);
						add_length(&first_trail,signals[2]);
						merge_lengths(first_trail);
						add_length(&first_repeat_gap,signals[3]);
						merge_lengths(first_repeat_gap);
					}
					else if(count==6)
					{
						count_5repeats++;
						add_length(&first_headerp,signals[0]);
						merge_lengths(first_headerp);
						add_length(&first_headers,signals[1]);
						merge_lengths(first_headers);
						add_length(&first_repeatp,signals[2]);
						merge_lengths(first_repeatp);
						add_length(&first_repeats,signals[3]);
						merge_lengths(first_repeats);
						add_length(&first_trail,signals[4]);
						merge_lengths(first_trail);
						add_length(&first_repeat_gap,signals[5]);
						merge_lengths(first_repeat_gap);
					}
					else if(count>6)
					{
						int i;

						printf(".");fflush(stdout);
						count_signals++;
						add_length(&first_1lead,signals[0]);
						merge_lengths(first_1lead);
						add_length(&first_3lead,signals[0]);
						merge_lengths(first_3lead);
						add_length(&first_headerp,signals[0]);
						merge_lengths(first_headerp);
						add_length(&first_headers,signals[1]);
						merge_lengths(first_headers);
						for(i=2;i<count-2;i++)
						{
							if(i%2)
							{
								add_length(&first_space,signals[i]);
								merge_lengths(first_space);
							}
							else
							{
								add_length(&first_pulse,signals[i]);
								merge_lengths(first_pulse);
							}
						}
						add_length(&first_trail,signals[count-2]);
						merge_lengths(first_trail);
						lengths[count-2]++;
						add_length(&first_signal_length,sum-data);
						merge_lengths(first_signal_length);
					}
					count=0;
					sum=0;
				}
				else
				{
					fprintf(stderr,"%s: wrong gap\n",progname);
					remote->gap=0;
					retval=0;
					break;
				}
				if(count_signals>=SAMPLES)
				{
					printf("\n");
					get_scheme(remote);
					if(!get_header_length(remote) ||
					   !get_trail_length(remote) ||
					   !get_lead_length(remote) ||
					   !get_repeat_length(remote) ||
					   !get_data_length(remote))
					{
						retval=0;
					}
					break;
				}
			}
		}
	}
	free_lengths(first_space);
	free_lengths(first_pulse);
	free_lengths(first_sum);
	free_lengths(first_gap);
	free_lengths(first_repeat_gap);
	free_lengths(first_signal_length);
	free_lengths(first_headerp);
	free_lengths(first_headers);
	free_lengths(first_1lead);
	free_lengths(first_3lead);
	free_lengths(first_trail);
	free_lengths(first_repeatp);
	free_lengths(first_repeats);
	return(retval);
}

/* handle lengths */

struct lengths *new_length(lirc_t length)
{
	struct lengths *l;

	l=malloc(sizeof(struct lengths));
	if(l==NULL) return(NULL);
	l->count=1;
	l->sum=length;
	l->lower_bound=length/100*100;
	l->upper_bound=length/100*100+99;
	l->min=l->max=length;
	l->next=NULL;
	return(l);
}

int add_length(struct lengths **first,lirc_t length)
{
	struct lengths *l,*last;

	if(*first==NULL)
	{
		*first=new_length(length);
		if(*first==NULL) return(0);
		return(1);
	}
	l=*first;
	while(l!=NULL)
	{
		if(l->lower_bound<=length && length<=l->upper_bound)
		{
			l->count++;
			l->sum+=length;
			l->min=min(l->min,length);
			l->max=max(l->max,length);
			return(1);
		}
		last=l;
		l=l->next;
	}
	last->next=new_length(length);
	if(last->next==NULL) return(0);
	return(1);
}

void free_lengths(struct lengths *first)
{
	struct lengths *next;

	if(first==NULL) return;
	while(first!=NULL)
	{
		next=first->next;
		free(first);
		first=next;
	}
}

void merge_lengths(struct lengths *first)
{
	struct lengths *l,*inner,*last;
	unsigned long new_sum;
	int new_count;

	l=first;
	while(l!=NULL)
	{
		last=l;
		inner=l->next;
		while(inner!=NULL)
		{
			new_sum=l->sum+inner->sum;
			new_count=l->count+inner->count;
			
			if((l->max<=new_sum/new_count+AEPS &&
			    l->min>=new_sum/new_count-AEPS &&
			    inner->max<=new_sum/new_count+AEPS &&
			    inner->min>=new_sum/new_count-AEPS)
			   ||
			   (l->max<=new_sum/new_count*(100+EPS) &&
			    l->min>=new_sum/new_count*(100-EPS) &&
			    inner->max<=new_sum/new_count*(100+EPS) &&
			    inner->min>=new_sum/new_count*(100-EPS)))
			{
				l->sum=new_sum;
				l->count=new_count;
				l->upper_bound=max(l->upper_bound,
						   inner->upper_bound);
				l->lower_bound=min(l->lower_bound,
						   inner->lower_bound);
				l->min=min(l->min,inner->min);
				l->max=max(l->max,inner->max);
				
				last->next=inner->next;
				free(inner);
				inner=last;
			}
			last=inner;
			inner=inner->next;
		}
		l=l->next;
	}
#       ifdef 0
	l=first;
	while(l!=NULL)
	{
		printf("%d x %lu [%lu,%lu]\n",l->count,
		       (unsigned long) l->sum/l->count,
		       (unsigned long) l->min,
		       (unsigned long) l->max);
		l=l->next;
	}
#       endif
}

void get_scheme(struct ir_remote *remote)
{
	unsigned int i,length=0,sum=0;

	for(i=1;i<MAX_SIGNALS;i++)
	{
		if(lengths[i]>lengths[length])
		{
			length=i;
		}
		sum+=lengths[i];
#               ifdef DEBUG
		if(lengths[i]>0) printf("%d: %lu\n",i,lengths[i]);
#               endif
	}
#       ifdef DEBUG
	printf("get_scheme(): sum: %u length: %u signals: %lu\n",
	       sum,length+1,lengths[length]);
#       endif
	if(lengths[length]>=TH_SPACE_ENC*sum/100)
	{
		length++;
		printf("Space/pulse encoded remote control found.\n");
		printf("Signal length is %u.\n",length);
		/* this is not yet the
		   number of bits */
		remote->bits=length;
		remote->flags|=SPACE_ENC;
	}
	else
	{
		printf("Shift encoded remote control found.\n");
		remote->flags|=SHIFT_ENC;
	}
}

struct lengths *get_max_length(struct lengths *first,unsigned int *sump)
{
	unsigned int sum;
	struct lengths *scan,*max_length;
	
	if(first==NULL) return(NULL);
	max_length=first;
	sum=first->count;
	
	scan=first->next;
	while(scan)
	{
		if(scan->count>max_length->count)
		{
			max_length=scan;
		}
		sum+=scan->count;
#               ifdef DEBUG
		if(scan->count>0) printf("%u x %lu\n",scan->count,
					 (unsigned long) scan->sum/scan->count);
#               endif
		scan=scan->next;
	}
	if(sump!=NULL) *sump=sum;
	return(max_length);
}

int get_trail_length(struct ir_remote *remote)
{
	unsigned int sum,max_count;
	struct lengths *max_length;

	if(is_shift(remote)) return(1);
	
	max_length=get_max_length(first_trail,&sum);
	max_count=max_length->count;
#       ifdef DEBUG
	printf("get_trail_length(): sum: %u, max_count %u\n",sum,max_count);
#       endif
	if(max_count>=sum*TH_TRAIL/100)
	{
		printf("Found trail pulse: %lu\n",
		       (unsigned long) (max_length->sum/max_length->count));
		remote->ptrail=max_length->sum/max_length->count;
		return(1);
	}
	printf("No trail pulse found.\n");
	return(1);
}

int get_lead_length(struct ir_remote *remote)
{
	unsigned int sum,max_count;
	struct lengths *max_length;

	if(!is_shift(remote)) return(1);
	
	max_length=get_max_length(has_header(remote) ?
				  first_3lead:first_1lead,&sum);
	max_count=max_length->count;
#       ifdef DEBUG
	printf("get_lead_length(): sum: %u, max_count %u\n",sum,max_count);
#       endif
	if(max_count>=sum*TH_LEAD/100)
	{
		printf("Found lead pulse: %lu\n",
		       (unsigned long) (max_length->sum/max_length->count));
		remote->plead=max_length->sum/max_length->count;
		return(1);
	}
	printf("No lead pulse found.\n");
	return(1);
}

int get_header_length(struct ir_remote *remote)
{
	unsigned int sum,max_count;
	lirc_t headerp,headers;
	struct lengths *max_plength,*max_slength;

	max_plength=get_max_length(first_headerp,&sum);
	max_count=max_plength->count;
#       ifdef DEBUG
	printf("get_header_length(): sum: %u, max_count %u\n",sum,max_count);
#       endif

	if(max_count>=sum*TH_HEADER/100)
	{
		max_slength=get_max_length(first_headers,&sum);
		max_count=max_slength->count;
#               ifdef DEBUG
		printf("get_header_length(): sum: %u, max_count %u\n",
		       sum,max_count);
#               endif
		if(max_count>=sum*TH_HEADER/100)
		{
			headerp=max_plength->sum/max_plength->count;
			headers=max_slength->sum/max_slength->count;

			printf("Found possible header: %lu %lu\n",
			       (unsigned long) headerp,
			       (unsigned long) headers);
			remote->phead=headerp;
			remote->shead=headers;
			return(1);
		}
	}
	printf("No header found.\n");
	return(1);
}

int get_repeat_length(struct ir_remote *remote)
{
	unsigned int sum,max_count;
	lirc_t repeatp,repeats,repeat_gap;
	struct lengths *max_plength,*max_slength;

	if(!((count_3repeats>SAMPLES/2 ? 1:0) ^
	     (count_5repeats>SAMPLES/2 ? 1:0)))
	{
		if(count_3repeats>SAMPLES/2 || count_5repeats>SAMPLES/2)
		{
			printf("Repeat inconsitentcy.\n");
			return(0);
		}
		printf("No repeat code found.\n");
		return(1);
	}

	max_plength=get_max_length(first_repeatp,&sum);
	max_count=max_plength->count;
#       ifdef DEBUG
	printf("get_repeat_length(): sum: %u, max_count %u\n",sum,max_count);
#       endif
	
	if(max_count>=sum*TH_REPEAT/100)
	{
		max_slength=get_max_length(first_repeats,&sum);
		max_count=max_slength->count;
#               ifdef DEBUG
		printf("get_repeat_length(): sum: %u, max_count %u\n",
		       sum,max_count);
#               endif
		if(max_count>=sum*TH_REPEAT/100)
		{
			if(count_5repeats>count_3repeats &&
			   !has_header(remote))
			{
				printf("Repeat code has header,"
				       " but no header found!\n");
				return(0);
			}
			if(count_3repeats>count_5repeats &&
			   has_header(remote))
			{
				remote->flags|=NO_HEAD_REP;
			}
			repeatp=max_plength->sum/max_plength->count;
			repeats=max_slength->sum/max_slength->count;
			
			printf("Found repeat code: %lu %lu\n",
			       (unsigned long) repeatp,
			       (unsigned long) repeats);
			remote->prepeat=repeatp;
			remote->srepeat=repeats;
			if(!(remote->flags&CONST_LENGTH))
			{
				max_slength=get_max_length(first_repeat_gap,
							   NULL);
				repeat_gap=max_slength->sum/max_slength->count;
				printf("Found repeat gap: %lu\n",
				       (unsigned long) repeat_gap);
				remote->repeat_gap=repeat_gap;
				
			}
			return(1);
		}
	}
	printf("No repeat header found.\n");
	return(1);

}

void unlink_length(struct lengths **first,struct lengths *remove)
{
	struct lengths *last,*scan;

	if(remove==*first)
	{
		*first=remove->next;
		remove->next=NULL;
		return;
	}
	else
	{
		scan=(*first)->next;
		last=*first;
		while(scan)
		{
			if(scan==remove)
			{
				last->next=remove->next;
				remove->next=NULL;
				return;
			}
			last=scan;
			scan=scan->next;
		}
	}
	printf("unlink_length(): report this bug!\n");
}

int get_data_length(struct ir_remote *remote)
{
	unsigned int sum,max_count;
	lirc_t p1,p2,s1,s2;
	struct lengths *max_plength,*max_slength;
	struct lengths *max2_plength,*max2_slength;

	max_plength=get_max_length(first_pulse,&sum);
	max_count=max_plength->count;
#       ifdef DEBUG
	printf("get_data_length(): sum: %u, max_count %u\n",sum,max_count);
#       endif

	if(max_count>=sum*TH_IS_BIT/100)
	{
		unlink_length(&first_pulse,max_plength);

		max2_plength=get_max_length(first_pulse,NULL);
		if(max2_plength!=NULL)
		{
			if(max2_plength->count<max_count*TH_IS_BIT/100)
				max2_plength=NULL;
		}

#               ifdef DEBUG
		printf("Pulse canditates: ");
		printf("%u x %lu",max_plength->count,
		       (unsigned long) max_plength->sum/max_plength->count);
		if(max2_plength) printf(", %u x %lu",max2_plength->count,
					(unsigned long) max2_plength->sum/max2_plength->count);
		printf("\n");
#               endif

		max_slength=get_max_length(first_space,&sum);
		max_count=max_slength->count;
#               ifdef DEBUG
		printf("get_header_length(): sum: %u, max_count %u\n",
		       sum,max_count);
#               endif
		if(max_count>=sum*TH_IS_BIT/100)
		{
			unlink_length(&first_space,max_slength);
			
			max2_slength=get_max_length(first_space,NULL);
			if(max2_slength!=NULL)
			{
				if(max2_slength->count<max_count*TH_IS_BIT/100)
					max2_slength=NULL;
			}
			
#                       ifdef DEBUG
			printf("Space canditates: ");
			printf("%u x %lu",max_slength->count,
			       (unsigned long) max_slength->sum/max_slength->count);
			if(max2_slength) printf(", %u x %lu",
						max2_slength->count,
						(unsigned long) max2_slength->sum/max2_slength->count);
			printf("\n");
#                       endif


			remote->eps=EPS;
			remote->aeps=AEPS;
			if(is_shift(remote))
			{
				if(max2_plength==NULL || max2_slength==NULL)
				{
					printf("Unknown encoding found.\n");
					return(0);
				}
				printf("Signals are shift encoded.\n");
				p1=max_plength->sum/max_plength->count;
				p2=max2_plength->sum/max2_plength->count;
				s1=max_slength->sum/max_slength->count;
				s2=max2_slength->sum/max2_slength->count;
				
				remote->pone=(min(p1,p2)+max(p1,p2)/2)/2;
				remote->sone=(min(s1,s2)+max(s1,s2)/2)/2;
				remote->pzero=remote->pone;
				remote->szero=remote->sone;
			}
			else
			{
				if(max2_plength==NULL &&
				   max2_slength==NULL)
				{
					printf("No encoding found.\n");
					return(0);
				}
				if(max2_plength && max2_slength)
				{
					printf("Unknown encoding found.\n");
					return(0);
				}
				p1=max_plength->sum/max_plength->count;
				s1=max_slength->sum/max_slength->count;
				if(max2_plength)
				{
					p2=max2_plength->sum/
						max2_plength->count;
					printf("Signals are pulse encoded.\n");
					remote->pone=max(p1,p2);
					remote->sone=s1;
					remote->pzero=min(p1,p2);
					remote->szero=s1;
				}
				else
				{
					s2=max2_slength->sum/
						max2_slength->count;
					printf("Signals are space encoded.\n");
					remote->pone=p1;
					remote->sone=max(s1,s2);
					remote->pzero=p1;
					remote->szero=min(s1,s2);
				}
			}
			if(has_header(remote) &&
			   (!has_repeat(remote) || remote->flags&NO_HEAD_REP)
			   )
			{
				if(!is_shift(remote) &&
				   ((expect(remote,remote->phead,remote->pone) &&
				     expect(remote,remote->shead,remote->sone)) ||
				    (expect(remote,remote->phead,remote->pzero) &&
				     expect(remote,remote->shead,remote->szero))))
				{
					remote->phead=remote->shead=0;
					remote->flags&=~NO_HEAD_REP;
					printf("Removed header.\n");
				}
				if(is_shift(remote) &&
				   expect(remote,remote->shead,remote->sone))
				{
					remote->plead=remote->phead;
					remote->phead=remote->shead=0;
					remote->flags&=~NO_HEAD_REP;
					printf("Removed header.\n");
				}
			}
			if(is_shift(remote))
			{
				struct lengths *signal_length;
				lirc_t data_length;

				signal_length=get_max_length(first_signal_length,
							     NULL);
				data_length=signal_length->sum/
					signal_length->count-
					remote->plead-
					remote->phead-
					remote->shead+
					/* + 1/2 bit */
					(remote->pone+remote->sone)/2;
				remote->bits=data_length/
					(remote->pone+remote->sone);

			}
			else
			{
				remote->bits=(remote->bits-
					      (has_header(remote) ? 2:0)+
					      1-(remote->ptrail>0 ? 2:0))/2;
			}
			printf("Signal length is %d\n",remote->bits);
			free_lengths(max_plength);
			free_lengths(max_slength);
			return(1);
		}
		free_lengths(max_plength);
	}
	printf("Could not find data lengths.\n");
	return(0);
}

int get_gap_length(struct ir_remote *remote)
{
	struct lengths *gaps=NULL;
	struct timeval start,end,last;
	int count,flag;
	struct lengths *scan;
	int maxcount,lastmaxcount;
	lirc_t gap;
	
	remote->eps=EPS;
	remote->aeps=AEPS;

	count=0;flag=0;lastmaxcount=0;
	printf("Hold down an arbitrary button.\n");
	while(1)
	{
		while(availabledata())
		{
			hw.rec_func(NULL);
		}
		if(!waitfordata(10000000))
		{
			free_lengths(gaps);
			return(0);
		}
		gettimeofday(&start,NULL);
		while(availabledata())
		{
			hw.rec_func(NULL);
		}
		gettimeofday(&end,NULL);
		if(flag)
		{
			gap=time_elapsed(&last,&start);
			add_length(&gaps,gap);
			merge_lengths(gaps);
			maxcount=0;
			scan=gaps;
			while(scan)
			{
				maxcount=max(maxcount,
					     scan->count);
				if(scan->count>SAMPLES)
				{
					remote->gap=scan->sum/scan->count;
					/* this does not work very reliably */
					remote->gap+=100000;
					printf("\nFound gap length: %lu\n",
					       (unsigned long) remote->gap);
					free_lengths(gaps);
					return(1);
				}
				scan=scan->next;
			}
			if(maxcount>lastmaxcount)
			{
				lastmaxcount=maxcount;
				printf(".");fflush(stdout);
			}
		}
		else
		{
			flag=1;
		}
		last=end;
	}
	return(1);
}

void fprint_copyright(FILE *fout)
{
	/* As this program is distributed under GPL you could just
	   remove this copyright notice and the config files generated
	   with the modified program would automatically be covered by
	   the GPL. Although I am aware of this I will not prevent it.

	   I hope that nobody will do so because the license I put on
	   the config files is not really a restriction. Instead it
	   emphasizes the spirit of the GPL to make things available
	   to everybody. */

	fprintf(fout,
		"\n"
		"# Copyright (C) 1999 Christoph Bartelmus\n"
		"#\n"
		"# You may only use this file if you make it available to others,\n"
		"# i.e. if you send it to <lirc@bartelmus.de>\n");
}