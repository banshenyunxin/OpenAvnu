/*
Copyright (c) 2013 Katja Rohloff <Katja.Rohloff@uni-jena.de>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <pcap/pcap.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <sndfile.h>

#include "listener_mrp_client.h"

//#define DEBUG
#define PCAP
#define LIBSND

#define ETHERNET_HEADER_SIZE 18
#define SEVENTEEN22_HEADER_PART1_SIZE 4
#define STREAM_ID_SIZE 8
#define SEVENTEEN22_HEADER_PART2_SIZE 10
#define SIX1883_HEADER_SIZE 10
#define HEADER_SIZE ETHERNET_HEADER_SIZE + SEVENTEEN22_HEADER_PART1_SIZE + STREAM_ID_SIZE + SEVENTEEN22_HEADER_PART2_SIZE + SIX1883_HEADER_SIZE 

#define SAMPLES_PER_SECOND 48000
#define SAMPLES_PER_FRAME 6
#define CHANNELS 2

struct six1883_sample{
	uint8_t label;
	uint8_t value[3];
};

struct ethernet_header{
	u_char dst[6];
	u_char src[6];
	u_char stuff[4];
	u_char type[2];
};

typedef int (*process_msg) (char *buf, int buflen);

// global
pcap_t* handle;
u_char ETHER_TYPE[] = { 0x22, 0xf0 };
SNDFILE* snd_file;

#define VERSION_STR	"1.1"
static const char *version_str = "simple_listener v" VERSION_STR "\n"
    "Copyright (c) 2012, Intel Corporation\n";

static void help()
{
	fprintf(stderr, "\n"
		"Usage: listener [-h] -i interface -f file_name.wav"
		"\n"
		"Options:\n"
		"    -h  show this message\n"
		"    -i  specify interface for AVB connection\n"
		"    -f  set the name of the output wav-file\n" 
		"\n" "%s" "\n", version_str);
	exit(1);
}

void pcap_callback(u_char* args, const struct pcap_pkthdr* packet_header, const u_char* packet)
{
	unsigned char* test_stream_id;
	struct ethernet_header* eth_header;
	uint32_t *buf;
	uint32_t frame[2] = { 0 , 0 };
	int i;
	(void) args; /* unused */
	(void) packet_header; /* unused */

#ifdef DEBUG
	fprintf(stdout,"Got packet.\n");
#endif	

	eth_header = (struct ethernet_header*)(packet);

#ifdef DEBUG
	fprintf(stdout,"Ether Type: 0x%02x%02x\n", eth_header->type[0], eth_header->type[1]);
#endif

	if (0 == memcmp(ETHER_TYPE,eth_header->type,sizeof(eth_header->type)))
	{		
		test_stream_id = (unsigned char*)(packet + ETHERNET_HEADER_SIZE + SEVENTEEN22_HEADER_PART1_SIZE);

#ifdef DEBUG
		fprintf(stderr, "Received stream id: %02x%02x%02x%02x%02x%02x%02x%02x\n ",
			     test_stream_id[0], test_stream_id[1],
			     test_stream_id[2], test_stream_id[3],
			     test_stream_id[4], test_stream_id[5],
			     test_stream_id[6], test_stream_id[7]);
#endif

		if (0 == memcmp(test_stream_id, stream_id, sizeof(STREAM_ID_SIZE)))
		{

#ifdef DEBUG
			fprintf(stdout,"Stream ids matched.\n");
#endif
			buf = (uint32_t*) (packet + HEADER_SIZE);
			for(i = 0; i < SAMPLES_PER_FRAME * CHANNELS; i += 2)
			{	
				memcpy(&frame[0], &buf[i], sizeof(frame));

				frame[0] = ntohl(frame[0]);   /* convert to host-byte order */
				frame[1] = ntohl(frame[1]);
				frame[0] &= 0x00ffffff;       /* ignore leading label */
				frame[1] &= 0x00ffffff;
				frame[0] <<= 8;               /* left-align remaining PCM-24 sample */
				frame[1] <<= 8;

				sf_writef_int(snd_file, (const int *)frame, 1);
			}
		}	
	}
}

void sigint_handler(int signum)
{
	int ret;

	fprintf(stdout,"Received signal %d:leaving...\n", signum);

	if (0 != talker)
		send_leave();

	if (2 > control_socket)
	{
		close(control_socket);
		ret = mrp_disconnect();
		if (ret)
			printf("mrp_disconnect failed\n");
	}

#ifdef PCAP
	if (NULL != handle) 
	{
		pcap_breakloop(handle);
		pcap_close(handle);
	}
#endif
	
#ifdef LIBSND
	sf_write_sync(snd_file);
	sf_close(snd_file);
#endif
}

int main(int argc, char *argv[])
{
	char* file_name = NULL;
	char* dev = NULL;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program comp_filter_exp;		/* The compiled filter expression */
	char filter_exp[] = "ether dst 91:E0:F0:00:0e:80";	/* The filter expression */

	signal(SIGINT, sigint_handler);

	int c;
	while((c = getopt(argc, argv, "hi:f:")) > 0) 
	{
		switch (c) 
		{
		case 'h': 
			help();
			break;
		case 'i':
			dev = strdup(optarg);
			break;
		case 'f':
			file_name = strdup(optarg);
			break;
		default:
          		fprintf(stderr, "Unrecognized option!\n");
		}
	}

	if ((NULL == dev) || (NULL == file_name))
		help();

	if (create_socket())
	{
		fprintf(stderr, "Socket creation failed.\n");
		return errno;
	}

	report_domain_status();
	join_vlan();

	fprintf(stdout,"Waiting for talker...\n");
	await_talker();	

#ifdef DEBUG
	fprintf(stdout,"Send ready-msg...\n");
#endif
	send_ready();
		
#ifdef LIBSND
	SF_INFO* sf_info = (SF_INFO*)malloc(sizeof(SF_INFO));

	memset(sf_info, 0, sizeof(SF_INFO));
	
	sf_info->samplerate = SAMPLES_PER_SECOND;
	sf_info->channels = CHANNELS;
	sf_info->format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;

	if (0 == sf_format_check(sf_info))
	{
		fprintf(stderr, "Wrong format.");
		return -1;
	}
			
	if (NULL == (snd_file = sf_open(file_name, SFM_WRITE, sf_info)))
	{
		fprintf(stderr, "Could not create file.");
		return -1;
	}
	fprintf(stdout,"Created file called %s\n", file_name);	
#endif

#ifdef PCAP		
	/** session, get session handler */
	/* take promiscuous vs. non-promiscuous sniffing? (0 or 1) */
	handle = pcap_open_live(dev, BUFSIZ, 1, -1, errbuf);
	if (NULL == handle) 
	{
		fprintf(stderr, "Could not open device %s: %s\n", dev, errbuf);
		return -1;
	}

#ifdef DEBUG
	fprintf(stdout,"Got session handler.\n");
#endif
	/* compile and apply filter */
	if (-1 == pcap_compile(handle, &comp_filter_exp, filter_exp, 0, PCAP_NETMASK_UNKNOWN))
	{
		fprintf(stderr, "Could not parse filter %s: %s\n", filter_exp, pcap_geterr(handle));
		return -1;
	}

	if (-1 == pcap_setfilter(handle, &comp_filter_exp)) 
	{
		fprintf(stderr, "Could not install filter %s: %s\n", filter_exp, pcap_geterr(handle));
		return -1;
	}

#ifdef DEBUG
	fprintf(stdout,"Compiled and applied filter.\n");
#endif

	/** loop forever and call callback-function for every received packet */
	pcap_loop(handle, -1, pcap_callback, NULL);
#endif

	return 0;
}