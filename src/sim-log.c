/*
 * sim-log.c
 *
 * Handle the simmgr logging system
 *
 * Copyright 2016 Terence Kelleher. All rights reserved.
 *
 * The log file is created in the directory /var/www/html/simlogs
 * The filename is created from the scenario name and start time
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include "../include/simmgr.h"

#define SIMLOG_DIR		"/var/www/html/simlogs"
#define SIMLOG_NAME_LENGTH 128
#define MAX_TIME_STR	24
#define MAX_LINE_LEN	512


/*
 * FUNCTION:
 *		simlog_create
 *
 * ARGUMENTS:
 *
 *
 * RETURNS:
 *		On success, returns 0. On fail, returns -1.
 */
char simlog_file[SIMLOG_NAME_LENGTH];
FILE *simlog_fd;
int simlog_line;	// Last line written
int lock_held = 0;

int
simlog_create()
{
	struct tm tm;
	char timeStr[MAX_TIME_STR];
	
	// Format from status.scenario.start: "Thu Jun 16 09:31:53 2016"
	strptime(simmgr_shm->status.scenario.start, "%c", &tm);
	strftime(timeStr, MAX_TIME_STR, "%Y_%m_%d_%H:%M:%S", &tm );
	sprintf(simlog_file, "%s/%s_%s.log", SIMLOG_DIR, timeStr, simmgr_shm->status.scenario.active );
	
	(void)simlog_open(SIMLOG_MODE_CREATE );
	if ( simlog_fd )
	{
		simlog_line = 0;
		simmgr_shm->logfile.active = 1;
		simlog_write((char *)"Start" );
		simlog_close();
		sprintf(simmgr_shm->logfile.filename, "%s_%s.log", timeStr, simmgr_shm->status.scenario.active );
		return ( 0 );
	}
	else
	{
		simmgr_shm->logfile.active = 0;
		sprintf(simmgr_shm->logfile.filename, "%s", "" );
		simmgr_shm->logfile.lines_written = 0;
		return ( -1 );
	}
}

void
simlog_entry(char *msg )
{
	if ( strlen(simlog_file ) > 0 )
	{
		(void)simlog_open(SIMLOG_MODE_WRITE );
		(void)simlog_write(msg );
		(void)simlog_close();
	}
}

int
simlog_open(int rw )
{
	int sts;
	char buffer[512];
	int trycount;
	
	if ( simlog_fd )
	{
		log_message("", "simlog_open called with simlog_fd already set" );
		return ( -1 );
	}
	if ( lock_held )
	{
		log_message("", "simlog_open called with lock_held set" );
		return ( -1 );
	}
	if ( rw == SIMLOG_MODE_WRITE || rw == SIMLOG_MODE_CREATE )
	{
		// Take the lock
		trycount = 0;
		while ( ( sts = sem_trywait(&simmgr_shm->logfile.sema) ) != 0 )
		{
			if ( trycount++ > 50 )
			{
				// Could not get lock soon enough. Try again next time.
				log_message("", "simlog_open failed to take logfile mutex" );
				return ( -1 );
			}
			usleep(10000 );
		}
		lock_held = 1;
		if ( rw == SIMLOG_MODE_CREATE )
		{
			simlog_fd = fopen(simlog_file, "w" );
		}
		else
		{
			simlog_fd = fopen(simlog_file, "a" );
		}
		if ( ! simlog_fd )
		{
			sprintf(buffer, "simlog_open failed to open for write: %s : %s", 
				simlog_file, strerror(errno) );
			log_message("", buffer  );
			sem_post(&simmgr_shm->logfile.sema);
			lock_held = 0;
		}
	}
	else
	{
		simlog_fd = fopen(simlog_file, "r" );
		if ( ! simlog_fd )
		{
			sprintf(buffer, "simlog_open failed to open for read: %s", simlog_file );
			log_message("", buffer  );
		}
	}
	if ( ! simlog_fd )
	{
		return ( -1 );
	}
	return ( 0 );
}
	
int
simlog_write(char *msg )
{
	if ( ! simlog_fd )
	{
		log_message("", "simlog_write called with closed file" );
		return ( -1 );
	}
	if ( ! lock_held )
	{
		log_message("", "simlog_write called without lock held" );
		return ( -1 );
	}
	if ( strlen(msg) > MAX_LINE_LEN )
	{
		log_message("", "simlog_write overlength string" );
		return ( -1 );
	}
	
	fprintf(simlog_fd, "%s %s\n", simmgr_shm->status.scenario.runtime, msg );

	simlog_line++;
	simmgr_shm->logfile.lines_written = simlog_line;
	return ( simlog_line );
}

int
simlog_read(char *rbuf )
{
	sprintf(rbuf, "%s", "" );
	
	if ( ! simlog_fd )
	{
		log_message("", "simlog_read called with closed file" );
		return ( -1 );
	}
	fgets(rbuf, MAX_LINE_LEN, simlog_fd );
	return ( strlen(rbuf ) );
}

int
simlog_read_line(char *rbuf, int lineno )
{
	int line;
	char *sts;
	
	sprintf(rbuf, "%s", "" );
	
	if ( ! simlog_fd )
	{
		log_message("", "simlog_read_line called with closed file" );
		return ( -1 );
	}

	fseek(simlog_fd, 0, SEEK_SET );
	for ( line = 1 ; line <= lineno ; line++ )
	{
		sts = fgets(rbuf, MAX_LINE_LEN, simlog_fd );
		if ( !sts )
		{
			return ( -1 );
		}
	}
	
	return ( strlen(rbuf ) );
}

void
simlog_close()
{
	// Close the file
	if ( simlog_fd )
	{
		fclose(simlog_fd );
		simlog_fd = NULL;
	}
	
	// Release the MUTEX
	if ( lock_held )
	{
		sem_post(&simmgr_shm->logfile.sema);
		lock_held = 0;
	}
}

void
simlog_end()
{
	if ( simlog_fd == NULL )
	{
		simlog_open(SIMLOG_MODE_WRITE );
	}
	simlog_write((char *)"End" );
	simlog_close();
	simmgr_shm->logfile.active = 0;
}
	