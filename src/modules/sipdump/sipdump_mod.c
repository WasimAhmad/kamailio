/**
 * Copyright (C) 2017 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "../../core/sr_module.h"
#include "../../core/dprint.h"
#include "../../core/ut.h"
#include "../../core/pvar.h"
#include "../../core/pt.h"
#include "../../core/timer_proc.h"
#include "../../core/mod_fix.h"
#include "../../core/events.h"
#include "../../core/kemi.h"

#include "sipdump_write.h"

MODULE_VERSION

static int sipdump_enable = 1;
int sipdump_rotate = 7200;
static int sipdump_wait = 100;
static str sipdump_folder = str_init("/tmp");
static str sipdump_fprefix = str_init("kamailio-sipdump-");

static int mod_init(void);
static int child_init(int);
static void mod_destroy(void);

static int w_sipdump_send(sip_msg_t *msg, char *ptag, char *str2);

int sipdump_msg_received(sr_event_param_t *evp);
int sipdump_msg_sent(sr_event_param_t *evp);

/* clang-format off */
static cmd_export_t cmds[]={
	{"sipdump_send", (cmd_function)w_sipdump_send, 1, fixup_spve_null,
		0, ANY_ROUTE},
	{0, 0, 0, 0, 0, 0}
};

static param_export_t params[]={
	{"enable",   PARAM_INT,   &sipdump_enable},
	{"wait",     PARAM_INT,   &sipdump_wait},
	{"rotate",   PARAM_INT,   &sipdump_rotate},
	{"folder",   PARAM_STR,   &sipdump_folder},
	{"fprefix",  PARAM_STR,   &sipdump_fprefix},
	{0, 0, 0}
};

struct module_exports exports = {
	"sipdump",
	DEFAULT_DLFLAGS, /* dlopen flags */
	cmds,
	params,
	0,
	0,              /* exported MI functions */
	0,              /* exported pseudo-variables */
	0,              /* extra processes */
	mod_init,       /* module initialization function */
	0,              /* response function */
	mod_destroy,    /* destroy function */
	child_init      /* per child init function */
};
/* clang-format on */


/**
 * init module function
 */
static int mod_init(void)
{
	if(sipdump_file_init(&sipdump_folder, &sipdump_fprefix) < 0) {
		LM_ERR("cannot initialize storage file\n");
		return -1;
	}

	if(sipdump_list_init(sipdump_enable) < 0) {
		LM_ERR("cannot initialize internal structure\n");
		return -1;
	}

	register_basic_timers(1);
	sr_event_register_cb(SREV_NET_DATA_IN, sipdump_msg_received);
	sr_event_register_cb(SREV_NET_DATA_OUT, sipdump_msg_sent);

	return 0;
}

/**
 * @brief Initialize async module children
 */
static int child_init(int rank)
{
	int i;

	if(rank != PROC_MAIN)
		return 0;

	if(fork_basic_utimer(PROC_TIMER, "SIPDUMP WRITE TIMER", 1 /*socks flag*/,
			   sipdump_timer_exec, NULL, sipdump_wait /*usec*/)
				< 0) {
		LM_ERR("failed to register timer routine as process (%d)\n", i);
		return -1; /* error */
	}

	return 0;
}

/**
 * destroy module function
 */
static void mod_destroy(void)
{
	sipdump_list_destroy();
}

#define SIPDUMP_WBUF_SIZE 65536
static char _sipdump_wbuf[SIPDUMP_WBUF_SIZE];

typedef struct sipdump_info {
	str tag;
	str buf;
	int af;
	int proto;
	str src_ip;
	int src_port;
	str dst_ip;
	int dst_port;
} sipdump_info_t;

/**
 *
 */
int sipdump_buffer_write(sipdump_info_t *sdi, str *obuf)
{
	struct timeval tv;
	struct tm *ti;

	gettimeofday(&tv, NULL);
	ti = localtime(&tv.tv_sec);
	obuf->len = snprintf(_sipdump_wbuf, SIPDUMP_WBUF_SIZE,
		"====================\n"
		"v: 1.0\n"
		"tag: %.*s\n"
		"pid: %d\n"
		"process: %d\n"
		"time: %lu.%06lu\n"
		"date: %s"
		"~~~~~~~~~~~~~~~~~~~~\n"
		"%.*s"
		"||||||||||||||||||||\n",
		sdi->tag.len, sdi->tag.s,
		my_pid(),
		process_no,
		(unsigned long)tv.tv_sec, (unsigned long)tv.tv_usec,
		asctime(ti),
		sdi->buf.len, sdi->buf.s
	);
	obuf->s = _sipdump_wbuf;

	return 0;
}

/**
 *
 */
int ki_sipdump_send(sip_msg_t *msg, str *stag)
{
	str wdata;
	sipdump_info_t sdi;

	if(!sipdump_enabled())
		return 1;
	
	memset(&sdi, 0, sizeof(sipdump_info_t));

	sdi.buf.s = msg->buf;
	sdi.buf.len = msg->len;
	sdi.tag = *stag;

	if(sipdump_buffer_write(&sdi, &wdata)<0) {
		LM_ERR("failed to write to buffer\n");
		return -1;
	}

	if(sipdump_list_add(&wdata)<0) {
		LM_ERR("failed to add data to write list\n");
		return -1;
	}
	return 1;
}

/**
 *
 */
static int w_sipdump_send(sip_msg_t *msg, char *ptag, char *str2)
{
	str stag;

	if(!sipdump_enabled())
		return 1;

	if(fixup_get_svalue(msg, (gparam_t*)ptag, &stag)<0) {
		LM_ERR("failed to get tag parameter\n");
		return -1;
	}
	return ki_sipdump_send(msg, &stag);
}

/**
 * 
 */
int sipdump_msg_received(sr_event_param_t *evp)
{
	str wdata;
	sipdump_info_t sdi;

	if(!sipdump_enabled())
		return 0;

	memset(&sdi, 0, sizeof(sipdump_info_t));

	sdi.buf = *((str*)evp->data);
	sdi.tag.s = "rcv";
	sdi.tag.len = 3;

	if(sipdump_buffer_write(&sdi, &wdata)<0) {
		LM_ERR("failed to write to buffer\n");
		return -1;
	}

	if(sipdump_list_add(&wdata)<0) {
		LM_ERR("failed to add data to write list\n");
		return -1;
	}
	return 0;
}

/**
 * 
 */
int sipdump_msg_sent(sr_event_param_t *evp)
{
	str wdata;
	sipdump_info_t sdi;

	if(!sipdump_enabled())
		return 0;

	memset(&sdi, 0, sizeof(sipdump_info_t));

	sdi.buf = *((str*)evp->data);
	sdi.tag.s = "snd";
	sdi.tag.len = 3;

	if(sipdump_buffer_write(&sdi, &wdata)<0) {
		LM_ERR("failed to write to buffer\n");
		return -1;
	}

	if(sipdump_list_add(&wdata)<0) {
		LM_ERR("failed to add data to write list\n");
		return -1;
	}
	return 0;
}

/**
 *
 */
/* clang-format off */
static sr_kemi_t sr_kemi_sipdump_exports[] = {
	{ str_init("sipdump"), str_init("send"),
		SR_KEMIP_INT, ki_sipdump_send,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},

	{ {0, 0}, {0, 0}, 0, NULL, { 0, 0, 0, 0, 0, 0 } }
};
/* clang-format on */

/**
 *
 */
int mod_register(char *path, int *dlflags, void *p1, void *p2)
{
	sr_kemi_modules_add(sr_kemi_sipdump_exports);
	return 0;
}