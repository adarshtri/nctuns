/*
 * Copyright (c) from 2000 to 2009
 * 
 * Network and System Laboratory 
 * Department of Computer Science 
 * College of Computer Science
 * National Chiao Tung University, Taiwan
 * All Rights Reserved.
 * 
 * This source code file is part of the NCTUns 6.0 network simulator.
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation is hereby granted (excluding for commercial or
 * for-profit use), provided that both the copyright notice and this
 * permission notice appear in all copies of the software, derivative
 * works, or modified versions, and any portions thereof, and that
 * both notices appear in supporting documentation, and that credit
 * is given to National Chiao Tung University, Taiwan in all publications 
 * reporting on direct or indirect use of this code or its derivatives.
 *
 * National Chiao Tung University, Taiwan makes no representations 
 * about the suitability of this software for any purpose. It is provided 
 * "AS IS" without express or implied warranty.
 *
 * A Web site containing the latest NCTUns 6.0 network simulator software 
 * and its documentations is set up at http://NSL.csie.nctu.edu.tw/nctuns.html.
 *
 * Project Chief-Technology-Officer
 * 
 * Prof. Shie-Yuan Wang <shieyuan@csie.nctu.edu.tw>
 * National Chiao Tung University, Taiwan
 *
 * 09/01/2009
 */

/*
 * A simple rtp_receive_only application for NCTUns.
 * We receive both RTP and RTCP packets, but only send RTCP packet to the address set by rtp_add_dest_list().
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netdb.h>
#include <unistd.h>

#include <rtp.h>
#include <rtp_session_api.h>


rtperror                err;                    /* the error that RTP lib returns */
session_id              sid;                    /* we can track the rtp by session_id (sid:session identify) */
protocol_t              proto = udp;            /* the protocol that rtp(rtcp) use */
socktype                rtp_sockt, rtcp_sockt;  /* track of socket, and assign to RTP, RTCP sockets */
u_int8                  ttl = 0;                /* it is only useful in mlticast that will be omit in unicast case */

double                  session_bw;		/* session bandwidth  */
char                    *local_ip, *remote_ip, *cname;
int                     local_port, remote_port, i = 0;
char                    sendbuf[RTP_MAX_PKT_SIZE], recvbuf[RTP_MAX_PKT_SIZE], *codec_name, marker = 1, *reason = "byebye!";
double                  rtp_start_time, rtp_stop_time, rtp_interval, starttime, nexttime, now;
int                     codec_num, i, packet_size, is_audio;
int                     addtimestamp, reads, nfds = 0, recbuflen = RTP_MAX_PKT_SIZE;
double                  bits_per_sample, delay, ms_pkt, sampling_rate, framerate;
struct  timeval         start_tv, now_tv, timeout_tv, nexttime_tv;
fd_set                  afds, rfds;

void			err_handle(rtperror err);


/* SDP : Session Description Protocol, RFC 2327.
 * we parse the SDP generated by NCTUns GUI, you can edit the sdp file by yourself and rewrite the sdp_parse() 
 * for parsing the sdp file */
void
sdp_parse(session_id sid, char **argv){

        FILE    *fp;
        char    line[50], *token;

        fp = fopen(argv[4], "r");
        if (fp) {
                while (fgets(line, 50, fp) != NULL) {   
                        token = strtok(line, "\t\n ");
                        if (!strncmp(token, "e=", 2)) { 		/* email address */
                                char    *tmp;
                                tmp = token + 2;
                                token = strtok(NULL, "\t\n ");
				
                                err = rtp_set_sdes_item(sid, RTCP_SDES_EMAIL, tmp, atoi(token));
                                if (err)
                                        err_handle(err);

                                printf("set email=%s with interval %d for sending SDES RTCP packet.\n", tmp, atoi(token));
                        }
                        else if (!strncmp(token, "p=", 2)) {    	/* phone number */
                                char    *tmp; 
                                tmp = token + 2;
                                token = strtok(NULL, "\t\n ");

                                err = rtp_set_sdes_item(sid, RTCP_SDES_PHONE, tmp, atoi(token));
                                if (err)
                                        err_handle(err);

                                printf("set phone=%s with interval %d for sending SDES RTCP packet.\n", tmp, atoi(token));
                        }
                        else if (!strncmp(token, "b=AS:", 5)) {
                                char    *tmp; 
                                tmp = token + 5;

                                session_bw = atof(tmp);
                                err = rtp_set_session_bw(sid, session_bw); /* set session bandwidth for this application */
                                if (err) 
                                        err_handle(err);
                                
                                printf("set session bandwidth=%lf\n", session_bw);
                        }
                        else if (!strncmp(token, "t=", 2)) {    /* time the session is active, NOTE! max_time is 4200 in NCTUns. */
                                char    *tmp; 

                                tmp = token + 2; 
                                rtp_start_time = atof(tmp);		/* the rtp session start time */

                                token = strtok(NULL, "\t\n ");
                                rtp_stop_time = atof(token);		/* the rtp session stop time */

                                printf("rtp_start_time = %lf, rtp_stop_time = %lf\n", rtp_start_time, rtp_stop_time);
                        }
                        else if (!strcmp(token, "m=audio")) {		/* media type is audio */
                                is_audio = 1;

                                token = strtok(NULL, "\t\n ");
                                remote_port = atoi(token);

                                token = strtok(NULL, "\t\n ");
                                token = strtok(NULL, "\t\n ");
                                codec_num = atoi(token);
                        }
                        else if (!strcmp(token, "m=video")) {		/* media type is video */
                                is_audio = 0;

                                token = strtok(NULL, "\t\n ");
                                remote_port = atoi(token);
                        }
                        else if (!strncmp(token, "a=rtpmap:", 9)) {	/* rtpmap */
                                char    *tmp, *temp, *rate;
                                tmp = token + 9;

                                codec_num = atoi(tmp);

                                token = strtok(NULL, "\t\n ");
                                tmp = strchr(token, '/');
                                codec_name = strndup(token, (tmp - token));

                                temp = strchr((tmp + 1), '/');
                                rate = strndup((tmp + 1), (temp - (tmp + 1)));
                                sampling_rate = atof(rate);
                                free(rate);

                                bits_per_sample = atof(temp + 1);
                                printf("codec_num = %d, codec_name = %s, sampling_rate = %lf, bits_per_sample = %lf\n", 
					codec_num, 
					codec_name, 
					sampling_rate, 
					bits_per_sample);
                        }
                        else if (!strncmp(token, "a=ptime:", 8)) {      /* ptime */
                                char    *tmp; 
                                tmp = token + 8;

                                ms_pkt = atof(tmp);
                                printf("ptime = %lf (ms)\n", ms_pkt);
                        }
                        else if (!strncmp(token, "a=framerate:", 12)) {	/* framerate */
                                char    *tmp; 
                                tmp = token + 12;

                                framerate = atof(tmp);
                                printf("frames/sec = %lf\n", framerate);
                        }
                        else if (!strcmp(token, "c=IN")) {		/* IN: internet */
                                char    *tmp; 
                                tmp = token + 4;
                                token = strtok(NULL, "\t\n ");
                                if (!strcmp(token, "IP4")) {    
                                        token = strtok(NULL, "\t\n ");
					/* add the destination address(es) */
                                        err = rtp_add_dest_list(sid, token, remote_port, ttl, proto, proto);
                                        if (err) 
                                                err_handle(err);
                                       
					printf("add the sending address %s\n", token);
                                }
                        }
                        else {  // unknown type, ignore it.
                        }
                }
        }
        else {
                fprintf(stderr, "Can't open file - %s , process terminated\n", argv[4]);
                exit(1);
        }
}


void 
err_handle(rtperror err){
	if (err < 0) {	/* warning */
		fprintf(stderr, "%s\n", RTPStrError(err));
	}
	else if (err) {	/* error */
		fprintf(stderr, "%s\n", RTPStrError(err));	
		exit(1);
	}		
}


/* set local receive addr, CNAME, and startup the connectoin */
void
initial_session(int argc, char **argv, session_id sid){
	
        /* setting commad line information */
        local_ip        = argv[1];
        local_port      = atoi(argv[2]);
        cname           = argv[3];

	/* set local receive addr */
        err = rtp_add_sour_list(sid, local_ip, local_port, proto, proto);
        if (err)
                err_handle(err);

	/* set CNAME. (see RFC 3550) */
        err = rtp_set_sdes_item(sid, RTCP_SDES_CNAME, cname, 1);	/* for CNAME SDES */
        if (err)
                err_handle(err);

        /* add the RTP Session's host address to send to and setup the socket. (NOTE : port != 0)*/
        err = rtp_open_connection(sid);
        if (err)
                err_handle(err);
}


/*
 * [ application_name local_ip local_port cname sdp_file [-t trace_file] ]
 */
int main(int argc, char **argv){

       /* create a rtp session, we will assign unique value to identify */
        err = rtp_create(&sid);
        if (err) {
                err_handle(err);
        }

        sdp_parse(sid, argv);	/* parse the sdp file */

	/* The application will sleep "rtp_start_time" time if "rtp_start_time" > 0 when the application is start. 
	 *
	 * NOTE! This is not the same with Start Time in NCTUns 
	 * EX:	if the Start Time in NCTUn is 5, and rtp_start_time is 3, 
	 * 	then the real time to startup the applicaion is 8.
	 */
        if (rtp_start_time > 0) 
                usleep( ((int) rtp_start_time * 1000000) );

        initial_session(argc, argv, sid);	/* set local receive addr, CNAME, and startup the connectoin */

        if (is_audio) 				/* media type is audio */
                delay   = (ms_pkt / 1000.);
        else 
                delay   = (1. / framerate);

	err = rtp_get_sour_rtpsocket(sid, &rtp_sockt);		/* get source rtp socket */
	if (err)
		err_handle(err);
	err = rtp_get_sour_rtcpsocket(sid, &rtcp_sockt);	/* get source rtcp socket */
	if (err)
		err_handle(err);
	
	if (rtp_sockt > nfds || rtcp_sockt > nfds) {
		if (rtp_sockt > rtcp_sockt)
			nfds = rtp_sockt;
		else
			nfds = rtcp_sockt;
	}
	
	FD_ZERO(&afds);
	FD_ZERO(&rfds);
	FD_SET(rtp_sockt, &afds);
	FD_SET(rtcp_sockt, &afds);
	
	gettimeofday(&start_tv, NULL);
	starttime = (start_tv.tv_sec + start_tv.tv_usec / 1000000.);
	now = starttime;
	
	rtp_interval = rtp_stop_time - rtp_start_time;
	
        /* bytes of each packet = ((bits/sample) / 8 ) * (clock rate) * ( each delay of packet in sec ) */
        // packet_size = (int) ( (bits_per_sample / 8.) * sampling_rate * delay);
	
	while ((now - starttime) <= rtp_interval) {
		
		memcpy(&rfds, &afds, sizeof(rfds));
		
		err = get_rtcp_timeout(sid, &timeout_tv);		/* get the send_rtcp_packet time */
		if (err) 
			err_handle(err);
		
		if (time_expire(&timeout_tv, &now_tv)) { 
				
			err = rtp_check_on_expire();			/* rtcp on_expire */
			if (err) 
				err_handle(err);
			
			err = get_rtcp_timeout(sid, &timeout_tv);	/* get the send_rtcp_packet time */
			if (err) 
				err_handle(err);
		}
			
		nexttime_tv = double_to_timeval(timeval_to_double(timeout_tv) - now);
			
		if (select(nfds + 1, &rfds, (fd_set *)0, (fd_set *)0, &nexttime_tv) < 0) {
			if (errno == EINTR)
				continue;
			else {	
				printf("nexttime_tv.tv_sec = %ld\n", nexttime_tv.tv_sec);
				printf("nexttime_tv.tv_usec = %ld\n", nexttime_tv.tv_usec);
				printf("select error: %d\n", errno);
			}
		}
		
		if (FD_ISSET(rtp_sockt, &rfds)) {
			err = on_receive(sid, rtp_sockt, recvbuf, &recbuflen);
			if (err) 
				err_handle(err);
		}
		else if (FD_ISSET(rtcp_sockt, &rfds)) {
			
			err = on_receive(sid, rtcp_sockt, recvbuf, &recbuflen);
			if (err) 
				err_handle(err);
		}
		
		gettimeofday(&now_tv, NULL);
		now = (now_tv.tv_sec + now_tv.tv_usec / 1000000.);
		
	} // while ((now - starttime) <= rtp_interval)
	
	err = rtp_close_connection(sid, reason);
	if (err) {
		err_handle(err);
	}
	
	err = rtp_delete(sid);
	if (err) {
		err_handle(err);
	}
				
	return 0;
}
