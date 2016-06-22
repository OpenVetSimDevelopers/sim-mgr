/*
 * simmgr.cpp
 *
 * SimMgr deamon.
 *
 * Copyright (C) 2016 Terence Kelleher. All rights reserved.
 *
 */
 
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <sys/timeb.h> 

#include <iostream>
#include <vector>  
#include <string>  
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>

#include "../include/simmgr.h" 

#define SCENARIO_LOOP_COUNT		25	// Run the scenario every SCENARIO_LOOP_COUNT iterations of the 10 msec loop
#define SCENARIO_TIMECHECK	(SCENARIO_LOOP_COUNT - 1)
#define SCENARIO_COMMCHECK  (SCENARIO_LOOP_COUNT - 10)

using namespace std;

int start_scenario(const char *name );
int scenarioPid = -1;

int scenario_run(void );
void comm_check(void );
void time_update(void );
int iiLockTaken = 0;
char buf[1024];
char msgbuf[2048];

// Time values, to track start time and elapsed time
std::time_t scenario_start_time;
std::time_t now;
std::time_t scenario_run_time;

enum ScenarioState { Stopped, Running, Paused, Terminate };
ScenarioState scenario_state = Stopped;

/* str_thdata
	structure to hold data to be passed to a thread
*/
typedef struct str_thdata
{
    int thread_no;
    char message[100];
} thdata;


/* prototype for thread routine */
void heart_thread ( void *ptr );

void
strToLower(char *buf )
{
	int i;
	for ( i = 0 ; buf[i] != 0 ; i++ )
	{
		buf[i] = (char)tolower(buf[i] );
	}
}

int
main(int argc, char *argv[] )
{
	int sts;
	char *ptr;
	int scenarioCount;
	daemonize();
	
	sts = initSHM(OPEN_WITH_CREATE );
	if ( sts < 0 )
	{
		perror("initSHM" );
		exit ( -1 );
	}
	
	// Zero out the shared memory and reinit the values
	memset(simmgr_shm, 0, sizeof(struct simmgr_shm) );

	// hdr
	simmgr_shm->hdr.version = SIMMGR_VERSION;
	simmgr_shm->hdr.size = sizeof(struct simmgr_shm);

	// server
	do_command_read("/bin/hostname", simmgr_shm->server.name, sizeof(simmgr_shm->server.name)-1 );
	ptr = getETH0_IP();
	sprintf(simmgr_shm->server.ip_addr, "%s", ptr );
	// server_time and msec_time are updated in the loop
	
	// status/cardiac
	sprintf(simmgr_shm->status.cardiac.rhythm, "%s", "sinus" );
	sprintf(simmgr_shm->status.cardiac.vpc, "%s", "none" );
	sprintf(simmgr_shm->status.cardiac.vfib_amplitude, "%s", "high" );
	simmgr_shm->status.cardiac.vpc_freq = 10;
	simmgr_shm->status.cardiac.pea = 0;
	simmgr_shm->status.cardiac.rate = 80;
	simmgr_shm->status.cardiac.nibp_rate = 80;
	sprintf(simmgr_shm->status.cardiac.pwave, "%s", "none" );
	simmgr_shm->status.cardiac.pr_interval = 140; // Good definition at http://lifeinthefastlane.com/ecg-library/basics/pr-interval/
	simmgr_shm->status.cardiac.qrs_interval = 85;
	simmgr_shm->status.cardiac.bps_sys = 105;
	simmgr_shm->status.cardiac.bps_dia = 70;
	simmgr_shm->status.cardiac.pulse_strength = 2;
	sprintf(simmgr_shm->status.cardiac.heart_sound, "%s", "none" );
	simmgr_shm->status.cardiac.heart_sound_volume = 10;
	simmgr_shm->status.cardiac.heart_sound_mute = 0;
	simmgr_shm->status.cardiac.ecg_indicator = 0;
	
	// status/respiration
	sprintf(simmgr_shm->status.respiration.left_lung_sound, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.left_sound_in, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.left_sound_out, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.left_sound_back, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.right_lung_sound, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.right_sound_in, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.right_sound_out, "%s", "normal" );
	sprintf(simmgr_shm->status.respiration.right_sound_back, "%s", "normal" );
	simmgr_shm->status.respiration.left_lung_sound_volume = 10;
	simmgr_shm->status.respiration.left_lung_sound_mute = 1;
	simmgr_shm->status.respiration.right_lung_sound_volume = 10;
	simmgr_shm->status.respiration.right_lung_sound_mute = 0;
	simmgr_shm->status.respiration.inhalation_duration = 1350;
	simmgr_shm->status.respiration.exhalation_duration = 1050;
	simmgr_shm->status.respiration.rate = 20;
	simmgr_shm->status.respiration.spo2 = 95;
	simmgr_shm->status.respiration.etco2 = 34;
	simmgr_shm->status.respiration.etco2_indicator = 0;
	simmgr_shm->status.respiration.spo2_indicator = 0;
	simmgr_shm->status.respiration.chest_movement = 0;
	
	// status/vocals
	sprintf(simmgr_shm->status.vocals.filename, "%s", "" );
	simmgr_shm->status.vocals.repeat = 0;
	simmgr_shm->status.vocals.volume = 10;
	simmgr_shm->status.vocals.play = 0;
	simmgr_shm->status.vocals.mute = 0;
	
	// status/auscultation
	simmgr_shm->status.auscultation.side = 0;
	simmgr_shm->status.auscultation.row = 0;
	simmgr_shm->status.auscultation.col = 0;
	
	// status/pulse
	simmgr_shm->status.pulse.position = 0;
	
	// status/cpr
	simmgr_shm->status.cpr.last = 0;
	simmgr_shm->status.cpr.compression = 0;
	simmgr_shm->status.cpr.release = 0;
	
	// status/defibrillation
	simmgr_shm->status.defibrillation.last = 0;
	simmgr_shm->status.defibrillation.energy = 0;
	
	// status/general
	simmgr_shm->status.general.temperature = 1017;
	
	// status/scenario
	sprintf(simmgr_shm->status.scenario.active, "%s", "default" );
	sprintf(simmgr_shm->status.scenario.state, "%s", "Stopped" );
	
	// instructor/sema
	sem_init(&simmgr_shm->instructor.sema, 1, 1 ); // pshared =1, value =1
	iiLockTaken = 0;
	
	// instructor/cardiac
	sprintf(simmgr_shm->instructor.cardiac.rhythm, "%s", "" );
	simmgr_shm->instructor.cardiac.rate = -1;
	simmgr_shm->instructor.cardiac.nibp_rate = -1;
	sprintf(simmgr_shm->instructor.cardiac.pwave, "%s", "" );
	simmgr_shm->instructor.cardiac.pr_interval = -1;
	simmgr_shm->instructor.cardiac.qrs_interval = -1;
	simmgr_shm->instructor.cardiac.bps_sys = -1;
	simmgr_shm->instructor.cardiac.bps_dia = -1;
	simmgr_shm->instructor.cardiac.pea = -1;
	simmgr_shm->instructor.cardiac.vpc_freq = -1;
	sprintf(simmgr_shm->instructor.cardiac.vpc, "%s", "" );
	sprintf(simmgr_shm->instructor.cardiac.vfib_amplitude, "%s", "" );
	simmgr_shm->instructor.cardiac.pulse_strength = -1;
	sprintf(simmgr_shm->instructor.cardiac.heart_sound, "%s", "" );
	simmgr_shm->instructor.cardiac.heart_sound_volume = -1;
	simmgr_shm->instructor.cardiac.heart_sound_mute = -1;
	simmgr_shm->instructor.cardiac.ecg_indicator = -1;
	
	// instructor/scenario
	sprintf(simmgr_shm->instructor.scenario.active, "%s", "" );
	sprintf(simmgr_shm->instructor.scenario.state, "%s", "" );

	// The start times is ignored.
	
	// instructor/respiration
	sprintf(simmgr_shm->instructor.respiration.left_lung_sound, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.left_sound_in, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.left_sound_out, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.left_sound_back, "%s", "" );
	simmgr_shm->instructor.respiration.left_lung_sound_volume = -1;
	simmgr_shm->instructor.respiration.left_lung_sound_mute = -1;
	simmgr_shm->instructor.respiration.right_lung_sound_volume = -1;
	simmgr_shm->instructor.respiration.right_lung_sound_mute = -1;
	sprintf(simmgr_shm->instructor.respiration.right_lung_sound, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.right_sound_in, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.right_sound_out, "%s", "" );
	sprintf(simmgr_shm->instructor.respiration.right_sound_back, "%s", "" );
	simmgr_shm->instructor.respiration.inhalation_duration = -1;
	simmgr_shm->instructor.respiration.exhalation_duration = -1;
	simmgr_shm->instructor.respiration.rate = -1;
	simmgr_shm->instructor.respiration.spo2 = -1;
	simmgr_shm->instructor.respiration.etco2 = -1;
	simmgr_shm->instructor.respiration.etco2_indicator = -1;
	simmgr_shm->instructor.respiration.spo2_indicator = -1;
	simmgr_shm->instructor.respiration.chest_movement = -1;
	simmgr_shm->instructor.general.temperature = -1;

	sprintf(simmgr_shm->instructor.vocals.filename, "%s", "" );
	simmgr_shm->instructor.vocals.repeat = -1;
	simmgr_shm->instructor.vocals.volume = -1;
	simmgr_shm->instructor.vocals.play = -1;
	simmgr_shm->instructor.vocals.mute = -1;
	
	// Log File
	sem_init(&simmgr_shm->logfile.sema, 1, 1 ); // pshared =1, value =1
	simmgr_shm->logfile.active = 0;
	sprintf(simmgr_shm->logfile.filename, "%s", "" );
	simmgr_shm->logfile.lines_written = 0;
	
	// status/scenario
	// (void)start_scenario("default" );
	
	scenarioCount = 0;
	while ( 1 )
	{
		scenarioCount++;
		
		if ( scenarioCount == SCENARIO_COMMCHECK )
		{
			comm_check();
		}
		else if ( scenarioCount == SCENARIO_TIMECHECK )
		{
			time_update();
		}
		else if ( scenarioCount >= SCENARIO_LOOP_COUNT )
		{
			scenarioCount = 0;
			(void)scenario_run();
		}

		usleep(10000);	// Sleep for 10 msec
	}
}

int
updateScenarioState(ScenarioState new_state)
{
	int rval = true;
	
	if ( new_state != scenario_state )
	{
		if ( ( new_state == Terminate ) && ( ( scenario_state != Running ) && ( scenario_state != Paused )) )
		{
			rval = false;
		}
		else if ( ( new_state == Paused ) && ( ( scenario_state != Running ) && ( scenario_state != Paused )) )
		{
			rval = false;
		}
		else 
		{
			scenario_state = new_state;
			
			switch ( scenario_state )
			{
				case Stopped:
					sprintf(simmgr_shm->status.scenario.state, "Stopped" );
					break;
				case Running:
					sprintf(simmgr_shm->status.scenario.state, "Running" );
					break;
				case Paused:
					sprintf(simmgr_shm->status.scenario.state, "Paused" );
					break;
				case Terminate:
					sprintf(simmgr_shm->status.scenario.state, "Terminate" );
					(void)simlog_end();
					break;
				default:
					sprintf(simmgr_shm->status.scenario.state, "Unknown" );
					break;
			}
		}
	}
	return ( rval );
}

/*
 * time_update
 *
 * Get the localtime and write it as a string to the SHM data
 */ 
int last_time_sec = -1;

void
time_update(void )
{
	struct tm tm;
	struct timeb timeb;
	int hour;
	int min;
	int sec;
	
	(void)ftime(&timeb );
	(void)localtime_r(&timeb.time, &tm );
	(void)asctime_r(&tm, buf );
	sprintf(simmgr_shm->server.server_time, "%s", buf );
	simmgr_shm->server.msec_time = (((tm.tm_hour*60*60)+(tm.tm_min*60)+tm.tm_sec)*1000)+ timeb.millitm;
	
	if ( scenario_state != Stopped )
	{
		now = std::time(nullptr );
		sec = (int)difftime(now, scenario_start_time );
		min = (sec / 60);
		hour = min / 60;
		sec = sec%60;
		sprintf(simmgr_shm->status.scenario.runtime, "%02d:%02d:%02d", hour, min%60, sec);
	}
	if ( ( sec == 0 ) && ( last_time_sec != 0 ) )
	{
		// Do periodic Stats update every minute
		sprintf(buf, "VS: Temp: %0.1f; awRR: %d; HR: %d; BP: %d/%d; SPO2: %d; etCO2: %d mmHg; Probes: ECG: %s; SPO2: %s; ETCO2: %s",
			((double)simmgr_shm->status.general.temperature) / 10,
			simmgr_shm->status.respiration.rate,
			simmgr_shm->status.cardiac.rate,
			simmgr_shm->status.cardiac.bps_sys,
			simmgr_shm->status.cardiac.bps_dia,
			simmgr_shm->status.respiration.spo2,
			simmgr_shm->status.respiration.etco2,
			(simmgr_shm->status.cardiac.ecg_indicator == 1 ? "on" : "off"  ),
			(simmgr_shm->status.respiration.spo2_indicator == 1 ? "on" : "off"  ),
			(simmgr_shm->status.respiration.etco2_indicator == 1 ? "on" : "off"  )
		);
		simlog_entry(buf );
	}
	last_time_sec = sec;
}
/*
 * comm_check
 *
 * verify that the communications path to the SimCtl is open and ok.
 * If not, try to reestablish.
 */
void
comm_check(void )
{
	// TBD
}

/*
 * Cardiac Process
 *
 * Based on the rate and target selected, modify the pulse rate
 */
struct trend cardiacTrend;
struct trend respirationTrend;
struct trend sysTrend;
struct trend diaTrend;
struct trend tempTrend;
struct trend spo2Trend;
struct trend etco2Trend;

int 
clearTrend(struct trend *trend, int current )
{
	trend->end = current;
	trend->current = current;
	
	return ( trend->current );
}

/*
 * duration is in seconds
*/


int 
setTrend(struct trend *trend, int end, int current, int duration )
{
	double diff;
	
	trend->end = (double)end;
	diff = (double)abs(end - current);

	if ( ( duration > 0 ) && ( diff > 0 ) )
	{
		trend->current = (double)current;
		trend->changePerSecond = diff / duration;
		trend->nextTime = time(0) + 1;
	}
	else
	{
		trend->current = end;
		trend->changePerSecond = 0;
		trend->nextTime = 0;
	}
	return ( (int)trend->current );
}

int
trendProcess(struct trend *trend )
{
	time_t now;
	double newval;
	int rval;
	
	now = time(0);
	
	if ( trend->nextTime && ( trend->nextTime <= now ) )
	{
		if ( trend->end > trend->current )
		{
			newval = trend->current + trend->changePerSecond;
			if ( newval > trend->end )
			{
				newval = trend->end;
			}
		}
		else
		{
			newval = trend->current - trend->changePerSecond;
			if ( newval < trend->end )
			{
				newval = trend->end;
			}
		}
		trend->current = newval;
		if ( trend->current == trend->end )
		{
			trend->nextTime = 0;
		}
		else
		{
			trend->nextTime = now + 1;
		}
	}
	rval = (int)round(trend->current );
	return ( rval );
}


/*
 * Scenario execution
 *
 * Reads II commands and changes operating parameters
 */
int
scenario_run(void )
{
	int sts;
	int trycount;
	int oldRate;
	int newRate;
	int period;
	
	// Lock the command interface before processing commands
	// Release the MUTEX
	
	trycount = 0;
	while ( ( sts = sem_trywait(&simmgr_shm->instructor.sema) ) != 0 )
	{
		if ( trycount++ > 50 )
		{
			// Could not get lock soon enough. Try again next time.
			return ( -1 );
		}
		usleep(100000 );
	}
	iiLockTaken = 1;
	
	// Check for instructor commands
	
	// Scenario
	if ( strlen(simmgr_shm->instructor.scenario.state ) > 0 )
	{
		strToLower(simmgr_shm->instructor.scenario.state );
		
		if ( strcmp(simmgr_shm->instructor.scenario.state, "paused" ) == 0 )
		{
			if ( scenario_state == Running )
			{
				updateScenarioState(Paused );
			}
		}
		else if ( strcmp(simmgr_shm->instructor.scenario.state, "running" ) == 0 )
		{
			if ( scenario_state == Paused )
			{
				updateScenarioState(Running );
			}
			else if ( scenario_state == Stopped )
			{
				sts = start_scenario(simmgr_shm->status.scenario.active );
			}
		}
		else if ( strcmp(simmgr_shm->instructor.scenario.state, "terminate" ) == 0 )
		{
			if ( (  scenario_state == Running ) || ( scenario_state == Paused ) )
			{
				updateScenarioState(Terminate );
			}
		}
		sprintf(simmgr_shm->instructor.scenario.state, "%s", "" );
	}
	if ( strlen(simmgr_shm->instructor.scenario.active) > 0 )
	{
		if ( scenario_state != Terminate )
		{
			if ( scenario_state == Stopped )
			{
				/*
				sts = start_scenario(simmgr_shm->instructor.scenario.active );
				if ( sts )
				{
					// Start Failure 
					
					// TODO: Add a failure message back to the Instructor
				}
				*/
			}
			sprintf(simmgr_shm->instructor.scenario.active, "%s", "" );
		}
	}
	
	// Cardiac
	if ( strlen(simmgr_shm->instructor.cardiac.rhythm ) > 0 )
	{
		if ( strcmp(simmgr_shm->status.cardiac.rhythm, simmgr_shm->instructor.cardiac.rhythm ) != 0 )
		{
			sprintf(simmgr_shm->status.cardiac.rhythm, "%s", simmgr_shm->instructor.cardiac.rhythm );
			sprintf(buf, "%s: %s", "Cardiac Rhythm", simmgr_shm->instructor.cardiac.rhythm );
			simlog_entry(buf );
		}
		sprintf(simmgr_shm->instructor.cardiac.rhythm, "%s", "" );
	}
	if ( simmgr_shm->instructor.cardiac.rate >= 0 )
	{
		if ( simmgr_shm->instructor.cardiac.rate != simmgr_shm->status.cardiac.rate )
		{
			simmgr_shm->status.cardiac.rate = setTrend(&cardiacTrend, 
											simmgr_shm->instructor.cardiac.rate,
											simmgr_shm->status.cardiac.rate,
											simmgr_shm->instructor.cardiac.transfer_time );
			if ( simmgr_shm->instructor.cardiac.transfer_time > 0 )
			{
				sprintf(buf, "%s: %d time %d", "Cardiac Rate", simmgr_shm->instructor.cardiac.rate, simmgr_shm->instructor.cardiac.transfer_time );
			}
			else
			{
				sprintf(buf, "%s: %d", "Cardiac Rate", simmgr_shm->instructor.cardiac.rate );
			}
			simlog_entry(buf );
		}
		simmgr_shm->instructor.cardiac.rate = -1;
	}
	if ( simmgr_shm->instructor.cardiac.nibp_rate >= 0 )
	{
		if ( simmgr_shm->status.cardiac.nibp_rate != simmgr_shm->instructor.cardiac.nibp_rate )
		{
			simmgr_shm->status.cardiac.nibp_rate = simmgr_shm->instructor.cardiac.nibp_rate;
			sprintf(buf, "%s: %d", "NIBP Rate", simmgr_shm->instructor.cardiac.rate );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.cardiac.nibp_rate = -1;
	}
	if ( strlen(simmgr_shm->instructor.cardiac.pwave ) > 0 )
	{
		sprintf(simmgr_shm->status.cardiac.pwave, "%s", simmgr_shm->instructor.cardiac.pwave );
		sprintf(simmgr_shm->instructor.cardiac.pwave, "%s", "" );
	}
	if ( simmgr_shm->instructor.cardiac.pr_interval >= 0 )
	{
		simmgr_shm->status.cardiac.pr_interval = simmgr_shm->instructor.cardiac.pr_interval;
		simmgr_shm->instructor.cardiac.pr_interval = -1;
	}
	if ( simmgr_shm->instructor.cardiac.qrs_interval >= 0 )
	{
		simmgr_shm->status.cardiac.qrs_interval = simmgr_shm->instructor.cardiac.qrs_interval;
		simmgr_shm->instructor.cardiac.qrs_interval = -1;
	}
	if ( simmgr_shm->instructor.cardiac.qrs_interval >= 0 )
	{
		simmgr_shm->status.cardiac.qrs_interval = simmgr_shm->instructor.cardiac.qrs_interval;
		simmgr_shm->instructor.cardiac.qrs_interval = -1;
	}
	if ( simmgr_shm->instructor.cardiac.bps_sys >= 0 )
	{
		simmgr_shm->status.cardiac.bps_sys = setTrend(&sysTrend, 
											simmgr_shm->instructor.cardiac.bps_sys,
											simmgr_shm->status.cardiac.bps_sys,
											simmgr_shm->instructor.cardiac.transfer_time );
		simmgr_shm->instructor.cardiac.bps_sys = -1;
	}
	if ( simmgr_shm->instructor.cardiac.bps_dia >= 0 )
	{
		simmgr_shm->status.cardiac.bps_dia = setTrend(&diaTrend, 
											simmgr_shm->instructor.cardiac.bps_dia,
											simmgr_shm->status.cardiac.bps_dia,
											simmgr_shm->instructor.cardiac.transfer_time );
		simmgr_shm->instructor.cardiac.bps_dia = -1;
	}
	if ( simmgr_shm->instructor.cardiac.pea >= 0 )
	{
		simmgr_shm->status.cardiac.pea = simmgr_shm->instructor.cardiac.pea;
		simmgr_shm->instructor.cardiac.pea = -1;
	}	
	if ( simmgr_shm->instructor.cardiac.pulse_strength >= 0 )
	{
		simmgr_shm->status.cardiac.pulse_strength = simmgr_shm->instructor.cardiac.pulse_strength;
		simmgr_shm->instructor.cardiac.pulse_strength = -1;
	}	
	if ( simmgr_shm->instructor.cardiac.vpc_freq >= 0 )
	{
		simmgr_shm->status.cardiac.vpc_freq = simmgr_shm->instructor.cardiac.vpc_freq;
		simmgr_shm->instructor.cardiac.vpc_freq = -1;
	}
	if ( strlen(simmgr_shm->instructor.cardiac.vpc) > 0 )
	{
		sprintf(simmgr_shm->status.cardiac.vpc, "%s", simmgr_shm->instructor.cardiac.vpc );
		sprintf(simmgr_shm->instructor.cardiac.vpc, "%s", "" );
	}
	if ( strlen(simmgr_shm->instructor.cardiac.vfib_amplitude) > 0 )
	{
		sprintf(simmgr_shm->status.cardiac.vfib_amplitude, "%s", simmgr_shm->instructor.cardiac.vfib_amplitude );
		sprintf(simmgr_shm->instructor.cardiac.vfib_amplitude, "%s", "" );
	}
	if ( strlen(simmgr_shm->instructor.cardiac.heart_sound) > 0 )
	{
		sprintf(simmgr_shm->status.cardiac.heart_sound, "%s", simmgr_shm->instructor.cardiac.heart_sound );
		sprintf(simmgr_shm->instructor.cardiac.heart_sound, "%s", "" );
	}
	if ( simmgr_shm->instructor.cardiac.heart_sound_volume >= 0 )
	{
		simmgr_shm->status.cardiac.heart_sound_volume = simmgr_shm->instructor.cardiac.heart_sound_volume;
		simmgr_shm->instructor.cardiac.heart_sound_volume = -1;
	}
	if ( simmgr_shm->instructor.cardiac.heart_sound_mute >= 0 )
	{
		simmgr_shm->status.cardiac.heart_sound_mute = simmgr_shm->instructor.cardiac.heart_sound_mute;
		simmgr_shm->instructor.cardiac.heart_sound_mute = -1;
	}
	
	if ( simmgr_shm->instructor.cardiac.ecg_indicator >= 0 )
	{
		if ( simmgr_shm->status.cardiac.ecg_indicator != simmgr_shm->instructor.cardiac.ecg_indicator )
		{
			simmgr_shm->status.cardiac.ecg_indicator = simmgr_shm->instructor.cardiac.ecg_indicator;
			sprintf(buf, "%s %s", "ECG Probe", (simmgr_shm->status.cardiac.ecg_indicator == 1 ? "Attached": "Removed") );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.cardiac.ecg_indicator = -1;
	}
	simmgr_shm->instructor.cardiac.transfer_time = -1;
	
	// Respiration
	if ( strlen(simmgr_shm->instructor.respiration.left_lung_sound) > 0 )
	{
		sprintf(simmgr_shm->status.respiration.left_lung_sound, "%s", simmgr_shm->instructor.respiration.left_lung_sound );
		sprintf(simmgr_shm->instructor.respiration.left_lung_sound, "%s", "" );
	}
	if ( strlen(simmgr_shm->instructor.respiration.right_lung_sound ) > 0 )
	{
		sprintf(simmgr_shm->status.respiration.right_lung_sound, "%s", simmgr_shm->instructor.respiration.right_lung_sound );
		sprintf(simmgr_shm->instructor.respiration.right_lung_sound, "%s", "" );
	}
	if ( simmgr_shm->instructor.respiration.inhalation_duration >= 0 )
	{
		simmgr_shm->status.respiration.inhalation_duration = simmgr_shm->instructor.respiration.inhalation_duration;
		simmgr_shm->instructor.respiration.inhalation_duration = -1;
	}
	if ( simmgr_shm->instructor.respiration.exhalation_duration >= 0 )
	{
		simmgr_shm->status.respiration.exhalation_duration = simmgr_shm->instructor.respiration.exhalation_duration;
		simmgr_shm->instructor.respiration.exhalation_duration = -1;
	}
	if ( simmgr_shm->instructor.respiration.left_lung_sound_volume >= 0 )
	{
		simmgr_shm->status.respiration.left_lung_sound_volume = simmgr_shm->instructor.respiration.left_lung_sound_volume;
		simmgr_shm->instructor.respiration.left_lung_sound_volume = -1;
	}
	if ( simmgr_shm->instructor.respiration.left_lung_sound_mute >= 0 )
	{
		simmgr_shm->status.respiration.left_lung_sound_mute = simmgr_shm->instructor.respiration.left_lung_sound_mute;
		simmgr_shm->instructor.respiration.left_lung_sound_mute = -1;
	}
	if ( simmgr_shm->instructor.respiration.right_lung_sound_volume >= 0 )
	{
		simmgr_shm->status.respiration.right_lung_sound_volume = simmgr_shm->instructor.respiration.right_lung_sound_volume;
		simmgr_shm->instructor.respiration.right_lung_sound_volume = -1;
	}
	if ( simmgr_shm->instructor.respiration.right_lung_sound_mute >= 0 )
	{
		simmgr_shm->status.respiration.right_lung_sound_mute = simmgr_shm->instructor.respiration.right_lung_sound_mute;
		simmgr_shm->instructor.respiration.right_lung_sound_mute = -1;
	}
	if ( simmgr_shm->instructor.respiration.rate >= 0 )
	{
		simmgr_shm->status.respiration.rate = setTrend(&respirationTrend, 
											simmgr_shm->instructor.respiration.rate,
											simmgr_shm->status.respiration.rate,
											simmgr_shm->instructor.respiration.transfer_time );
		simmgr_shm->instructor.respiration.rate = -1;
	}
	
	if ( simmgr_shm->instructor.respiration.spo2 >= 0 )
	{
		simmgr_shm->status.respiration.spo2 = setTrend(&spo2Trend, 
											simmgr_shm->instructor.respiration.spo2,
											simmgr_shm->status.respiration.spo2,
											simmgr_shm->instructor.respiration.transfer_time );
		simmgr_shm->instructor.respiration.spo2 = -1;
	}
	
	if ( simmgr_shm->instructor.respiration.etco2 >= 0 )
	{
		simmgr_shm->status.respiration.etco2 = setTrend(&etco2Trend, 
											simmgr_shm->instructor.respiration.etco2,
											simmgr_shm->status.respiration.etco2,
											simmgr_shm->instructor.respiration.transfer_time );
		simmgr_shm->instructor.respiration.etco2 = -1;
	}
	if ( simmgr_shm->instructor.respiration.etco2_indicator >= 0 )
	{
		if ( simmgr_shm->status.respiration.etco2_indicator != simmgr_shm->instructor.respiration.etco2_indicator )
		{
			simmgr_shm->status.respiration.etco2_indicator = simmgr_shm->instructor.respiration.etco2_indicator;
			sprintf(buf, "%s %s", "ETCO2 Probe", (simmgr_shm->status.respiration.etco2_indicator == 1 ? "Attached": "Removed") );
			simlog_entry(buf );
		}
		
		simmgr_shm->instructor.respiration.etco2_indicator = -1;
	}
	if ( simmgr_shm->instructor.respiration.spo2_indicator >= 0 )
	{
		if ( simmgr_shm->status.respiration.spo2_indicator != simmgr_shm->instructor.respiration.spo2_indicator )
		{
			simmgr_shm->status.respiration.spo2_indicator = simmgr_shm->instructor.respiration.spo2_indicator;
			sprintf(buf, "%s %s", "SPO2 Probe", (simmgr_shm->status.respiration.spo2_indicator == 1 ? "Attached": "Removed") );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.respiration.spo2_indicator = -1;
	}
	if ( simmgr_shm->instructor.respiration.chest_movement >= 0 )
	{
		if ( simmgr_shm->status.respiration.chest_movement != simmgr_shm->instructor.respiration.chest_movement )
		{
			simmgr_shm->status.respiration.chest_movement = simmgr_shm->instructor.respiration.chest_movement;
		}
		simmgr_shm->instructor.respiration.chest_movement = -1;
	}
	simmgr_shm->instructor.respiration.transfer_time = -1;
	
	// General
	if ( simmgr_shm->instructor.general.temperature >= 0 )
	{
		simmgr_shm->status.general.temperature = setTrend(&tempTrend, 
											simmgr_shm->instructor.general.temperature,
											simmgr_shm->status.general.temperature,
											simmgr_shm->instructor.general.transfer_time );
		simmgr_shm->instructor.general.temperature = -1;
	}
	simmgr_shm->instructor.general.transfer_time = -1;
	
	// vocals
	if ( strlen(simmgr_shm->instructor.vocals.filename) > 0 )
	{
		sprintf(simmgr_shm->status.vocals.filename, "%s", simmgr_shm->instructor.vocals.filename );
		sprintf(simmgr_shm->instructor.vocals.filename, "%s", "" );
	}
	if ( simmgr_shm->instructor.vocals.repeat >= 0 )
	{
		simmgr_shm->status.vocals.repeat = simmgr_shm->instructor.vocals.repeat;
		simmgr_shm->instructor.vocals.repeat = -1;
	}
	if ( simmgr_shm->instructor.vocals.volume >= 0 )
	{
		simmgr_shm->status.vocals.volume = simmgr_shm->instructor.vocals.volume;
		simmgr_shm->instructor.vocals.volume = -1;
	}
	if ( simmgr_shm->instructor.vocals.play >= 0 )
	{
		simmgr_shm->status.vocals.play = simmgr_shm->instructor.vocals.play;
		simmgr_shm->instructor.vocals.play = -1;
	}
	if ( simmgr_shm->instructor.vocals.mute >= 0 )
	{
		simmgr_shm->status.vocals.mute = simmgr_shm->instructor.vocals.mute;
		simmgr_shm->instructor.vocals.mute = -1;
	}
	
	// Release the MUTEX
	sem_post(&simmgr_shm->instructor.sema);
	iiLockTaken = 0;
	
	if ( scenario_state == Running )
	{
		// Process the trends
		simmgr_shm->status.cardiac.rate = trendProcess(&cardiacTrend );
		simmgr_shm->status.cardiac.bps_sys = trendProcess(&sysTrend );
		simmgr_shm->status.cardiac.bps_dia = trendProcess(&diaTrend );
		oldRate = simmgr_shm->status.respiration.rate;
		newRate = trendProcess(&respirationTrend );
		if ( oldRate != newRate )
		{
			if ( newRate > 0 )
			{
				simmgr_shm->status.respiration.rate = newRate;
				period = (1000*60)/newRate;	// Period in msec from rate per minute
				simmgr_shm->status.respiration.inhalation_duration = period / 2;
				simmgr_shm->status.respiration.exhalation_duration = period - simmgr_shm->status.respiration.inhalation_duration;
			}
			else
			{
				simmgr_shm->status.respiration.rate = 0;
				simmgr_shm->status.respiration.inhalation_duration = 0;
				simmgr_shm->status.respiration.exhalation_duration = 0;
			}
		}
		simmgr_shm->status.respiration.spo2 = trendProcess( &spo2Trend );
		simmgr_shm->status.respiration.etco2 = trendProcess( &etco2Trend );
		simmgr_shm->status.general.temperature = trendProcess(&tempTrend );
	}
	else if ( scenario_state == Terminate )
	{
		if ( simmgr_shm->logfile.active == 0 )
		{
			updateScenarioState(Stopped );
		}
	}
	return ( 0 );
}


int
start_scenario(const char *name )
{
	char timeBuf[64];
	char fname[128];

	sprintf(msgbuf, "Start Scenario Request: %s", name );
	log_message("", msgbuf ); 
	sprintf(fname, "%s/%s.xml", "/var/www/html/scenarios", name );
	
	scenario_start_time = std::time(nullptr );
	std::strftime(timeBuf, 60, "%c", std::localtime(&scenario_start_time ));
	
	// Clear running trends
	(void)clearTrend(&cardiacTrend, simmgr_shm->status.cardiac.rate );
	(void)clearTrend(&sysTrend, simmgr_shm->status.cardiac.bps_sys  );
	(void)clearTrend(&diaTrend, simmgr_shm->status.cardiac.bps_dia  );
	(void)clearTrend(&respirationTrend, simmgr_shm->status.respiration.rate );
	(void)clearTrend(&spo2Trend, simmgr_shm->status.respiration.spo2 );
	(void)clearTrend(&etco2Trend, simmgr_shm->status.respiration.etco2 );
	(void)clearTrend(&tempTrend, simmgr_shm->status.general.temperature );
	
	// exec the new scenario
	scenarioPid = fork();
	if ( scenarioPid == 0 )
	{
		// Child
		
		sprintf(msgbuf, "Start Scenario: execl %s  %s", "/usr/local/bin/scenario", fname );
		log_message("", msgbuf ); 
		
		execl("/usr/local/bin/scenario", "scenario", fname, (char *)0 );
		// execl does not return on success 
		sprintf(msgbuf, "Start Scenario: execl fails for %s :%s", name, strerror(errno ) );
		log_message("", msgbuf ); 
	}
	else if ( scenarioPid > 0 )
	{
		// Parent
		sprintf(msgbuf, "Start Scenario: %s Pid is %d", name, scenarioPid );
		log_message("", msgbuf ); 
		sprintf(simmgr_shm->status.scenario.active, "%s", name );
		
		sprintf(simmgr_shm->status.scenario.start, "%s", timeBuf );
		sprintf(simmgr_shm->status.scenario.runtime, "%s", "00:00:00" );
		//sprintf(simmgr_shm->status.scenario.scene_name, "%s", "init" );
		//simmgr_shm->status.scenario.scene_id = 0;
		
		updateScenarioState(Running );
		(void)simlog_create();
	}
	else
	{
		// Failed
		sprintf(msgbuf, "fork Fails %s", strerror(errno) );
		log_message("", msgbuf );
	}

	return ( 0 );
}