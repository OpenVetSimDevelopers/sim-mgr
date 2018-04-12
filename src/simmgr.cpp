/*
 * simmgr.cpp
 *
 * SimMgr daemon.
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

#define SCENARIO_LOOP_COUNT		20	// Run the scenario every SCENARIO_LOOP_COUNT iterations of the 10 msec loop
#define SCENARIO_COMMCHECK  	5
#define SCENARIO_EVENTCHECK		10
#define SCENARIO_AWRRCHECK		15
#define SCENARIO_TIMECHECK		19


using namespace std;

int start_scenario(const char *name );
void recordStartStop(int record );
void checkEvents(void );
void clearAllTrends(void );
void resetAllParameters(void );

int scenarioPid = -1;
int lastEventLogged = 0;
int lastCommentLogged = 0;
int runningAsDemo = 0;

int scan_commands(void );
void comm_check(void );
void time_update(void );
void awrr_check(void );
int iiLockTaken = 0;
char buf[1024];
char msgbuf[2048];

// Time values, to track start time and elapsed time
std::time_t scenario_start_time;
std::time_t now;
std::time_t scenario_run_time;


ScenarioState scenario_state = ScenarioStopped;
NibpState nibp_state = NibpIdle;
std::time_t nibp_next_time;
std::time_t nibp_run_complete_time;

/* str_thdata
	structure to hold data to be passed to a thread
*/
typedef struct str_thdata
{
    int thread_no;
    char message[100];
} thdata;


/* prototype for thread routines */
void heart_thread ( void *ptr );
void nibp_thread ( void *ptr );

void
strToLower(char *buf )
{
	int i;
	for ( i = 0 ; buf[i] != 0 ; i++ )
	{
		buf[i] = (char)tolower(buf[i] );
	}
}

void
killScenario(int arg1, void *arg2 )
{
	if ( scenarioPid > 0 )
	{
		kill( scenarioPid, SIGTERM );
		scenarioPid = -1;
	}
}

int
main(int argc, char *argv[] )
{
	int sts;
	char *ptr;
	int scenarioCount;
	daemonize();
	char sesid[512] = { 0, };
	
	sts = on_exit(killScenario, (void *)0 );
	
	sts = initSHM(OPEN_WITH_CREATE, sesid );
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
	ptr = getWIFI_IP();
	sprintf(simmgr_shm->server.wifi_ip_addr, "%s", ptr );
	// server_time and msec_time are updated in the loop
	
	resetAllParameters();

	// status/scenario
	sprintf(simmgr_shm->status.scenario.active, "%s", "default" );
	sprintf(simmgr_shm->status.scenario.state, "%s", "Stopped" );
	simmgr_shm->instructor.scenario.record = 0;
	
	// instructor/sema
	sem_init(&simmgr_shm->instructor.sema, 1, 1 ); // pshared =1, value =1
	iiLockTaken = 0;

	// instructor/scenario
	sprintf(simmgr_shm->instructor.scenario.active, "%s", "" );
	sprintf(simmgr_shm->instructor.scenario.state, "%s", "" );
	simmgr_shm->instructor.scenario.record = -1;
	
	// Log File
	sem_init(&simmgr_shm->logfile.sema, 1, 1 ); // pshared =1, value =1
	simmgr_shm->logfile.active = 0;
	sprintf(simmgr_shm->logfile.filename, "%s", "" );
	simmgr_shm->logfile.lines_written = 0;
	
	// Event List
	simmgr_shm->eventListNext = 0;
	lastEventLogged = 0;
	
	// Comment List
	simmgr_shm->commentListNext = 0;
	lastCommentLogged = 0;
	
	// instructor/cpr
	simmgr_shm->instructor.cpr.compression = -1;
	simmgr_shm->instructor.cpr.last = -1;
	simmgr_shm->instructor.cpr.release = -1;
	simmgr_shm->instructor.cpr.duration = -1;
	
	clearAllTrends();
	
	scenarioCount = 0;
#define SCENARIO_LOOP_COUNT		20	// Run the scenario every SCENARIO_LOOP_COUNT iterations of the 10 msec loop
#define SCENARIO_COMMCHECK  	5
#define SCENARIO_EVENTCHECK		10
#define SCENARIO_AWRRCHECK		15
#define SCENARIO_TIMECHECK		19
	while ( 1 )
	{
		scenarioCount++;
		
		switch ( scenarioCount )
		{
			case SCENARIO_COMMCHECK:
				comm_check();
				break;
			case SCENARIO_EVENTCHECK:
				checkEvents();
				break;
			case SCENARIO_AWRRCHECK:
				awrr_check();
				break;
			case SCENARIO_TIMECHECK:
				time_update();
				break;
			case SCENARIO_LOOP_COUNT:
				scenarioCount = 0;
				(void)scan_commands();
				break;
			default:
				break;
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
		if ( ( new_state == ScenarioTerminate ) && ( ( scenario_state != ScenarioRunning ) && ( scenario_state != ScenarioPaused )) )
		{
			rval = false;
		}
		else if ( ( new_state == ScenarioPaused ) && ( ( scenario_state != ScenarioRunning ) && ( scenario_state != ScenarioPaused )) )
		{
			rval = false;
		}
		else 
		{
			scenario_state = new_state;
			
			switch ( scenario_state )
			{
				case ScenarioStopped:
					sprintf(simmgr_shm->status.scenario.state, "Stopped" );
					(void)simlog_end();
					break;
				case ScenarioRunning:
					sprintf(simmgr_shm->status.scenario.state, "Running" );
					break;
				case ScenarioPaused:
					sprintf(simmgr_shm->status.scenario.state, "Paused" );
					break;
				case ScenarioTerminate:
					sprintf(simmgr_shm->status.scenario.state, "Terminate" );
					(void)simlog_end();
					break;
				default:
					sprintf(simmgr_shm->status.scenario.state, "Unknown" );
					break;
			}
			sprintf(msgbuf, "State: %s ", simmgr_shm->status.scenario.state );
			log_message("", msgbuf ); 
		}
	}
	return ( rval );
}

/*
 * awrr_check
 *
 * Calculate awrr based on count of breaths, both manual and 'normal'
 *
 * 1 - Detect and log breath when either natural or manual start
 * 2 - If no breaths are recorded in the past 20 seconds (excluding the past 2 seconds) report AWRR as zero
 * 3 - Calculate AWRR based on the average time of the recorded breaths within the past 90 seconds, excluding the past 2 seconds
 *
*/
#define BREATH_CALC_LIMIT		5		// Max number of recorded breaths to count in calculation
#define BREATH_LOG_LEN	128
int breathLog[BREATH_LOG_LEN] = { 0, };
int breathLogNext = 0;

#define BREATH_LOG_STATE_IDLE	0
#define BREATH_LOG_STATE_DETECT	1
int breathLogState = BREATH_LOG_STATE_IDLE;

unsigned int breathLogLastNatural = 0;	// breathCount, last natural
int breathLogLastManual = 0;	// manual_count, last manual
int breathLogLast = 0;			// Time of last breath

#define BREATH_LOG_DELAY	(2)
int breathLogDelay = 0;
void
awrr_check(void)
{
	int now = time(NULL);	// Current sec time
	int prev;
	int breaths;
	int totalTime;
	int lastTime;
	int firstTime;
	int diff;
	float awRR;
	int i;
	int intervals;
	
	// Breath Detect and Log
	if ( breathLogState == BREATH_LOG_STATE_DETECT )
	{
		// After detect, wait 400 msec (two calls) before another detect allowed
		if ( breathLogDelay++ >= BREATH_LOG_DELAY)
		{
			breathLogState = BREATH_LOG_STATE_IDLE;
			breathLogLastNatural = simmgr_shm->status.respiration.breathCount;
			breathLogLastManual = simmgr_shm->status.respiration.manual_count;
		}
	}
	else if ( breathLogState == BREATH_LOG_STATE_IDLE )
	{
		if ( breathLogLastNatural != simmgr_shm->status.respiration.breathCount )
		{
			breathLogState = BREATH_LOG_STATE_DETECT;
			breathLogLastNatural = simmgr_shm->status.respiration.breathCount;
			breathLogLast = now;
			breathLog[breathLogNext] = now;
			breathLogDelay = 0;
			breathLogNext += 1;
			if ( breathLogNext >= BREATH_LOG_LEN )
			{
				breathLogNext = 0;
			}
		}
		else if ( breathLogLastManual != simmgr_shm->status.respiration.manual_count )
		{
			breathLogState = BREATH_LOG_STATE_DETECT;
			breathLogLastManual = simmgr_shm->status.respiration.manual_count;
			breathLogLast = now;
			breathLog[breathLogNext] = now;
			breathLogDelay = 0;
			breathLogNext += 1;
			if ( breathLogNext >= BREATH_LOG_LEN )
			{
				breathLogNext = 0;
			}
		}
	}
	
	// AWRR Calculation - Look at no more than 10 breaths - Skip if no breaths within 20 seconds
	lastTime = 0;
	firstTime = 0;
	prev = breathLogNext - 1;
	if ( prev < 0 ) 
	{
		prev = BREATH_LOG_LEN - 1;
	}
	breaths = 0;
	intervals = 0;
	
	if ( breathLog[prev] <= 0 )  // Don't look at empty logs
	{
		simmgr_shm->status.respiration.awRR = 0;
	}
	else
	{
		lastTime = breathLog[prev];
		diff = now - lastTime;
		if ( diff > 20 )
		{
			simmgr_shm->status.respiration.awRR = 0;
		}
		else
		{
			prev -= 1;
			if ( prev < 0 ) 
			{
				prev = BREATH_LOG_LEN - 1;
			}
			for ( i = 0 ; i < BREATH_CALC_LIMIT ; i++ )
			{
				diff = now - breathLog[prev];
				if ( diff > 20 ) // Over Limit seconds since this recorded breath
				{
					break;
				}
				else
				{
					firstTime = breathLog[prev]; // Recorded start of the first breath
					breaths += 1;
					intervals += 1;
					prev -= 1;
					if ( prev < 0 ) 
					{
						prev = BREATH_LOG_LEN - 1;
					}
				}
			}
		}
	}
	simmgr_shm->server.dbg1 = lastTime;
	simmgr_shm->server.dbg2 = firstTime;
	simmgr_shm->server.dbg3 = intervals;
	
	if ( intervals > 0 )
	{
		totalTime = lastTime - firstTime;
		if ( totalTime == 0 )
		{
			awRR = 0;
		}
		else
		{
			awRR = ( (float)intervals / (float)totalTime ) * 60;
		}
		if ( awRR < 0 )
		{
			simmgr_shm->status.respiration.awRR = 0;
		}
		else if ( awRR > 60 )
		{
			simmgr_shm->status.respiration.awRR = 61;
		}
		else
		{
			simmgr_shm->status.respiration.awRR = (int)awRR;
		}
	}
	else
	{
		simmgr_shm->status.respiration.awRR = 0;
	}
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
	
	now = std::time(nullptr );
	sec = (int)difftime(now, scenario_start_time );
	if ( ( sec > MAX_SCENARIO_RUNTIME ) &&
		 ( ( scenario_state == ScenarioRunning ) || 
		   ( scenario_state == ScenarioPaused ) ) )
	{
		sprintf(buf, "MAX Scenario Runtime exceeded. Terminating." );
		simlog_entry(buf );
		
		takeInstructorLock();
		sprintf(simmgr_shm->instructor.scenario.state, "%s", "Terminate" );
		releaseInstructorLock();
	}
	else if ( scenario_state == ScenarioRunning )
	{
		min = (sec / 60);
		hour = min / 60;
		sec = sec%60;
		sprintf(simmgr_shm->status.scenario.runtime, "%02d:%02d:%02d", hour, min%60, sec);
	
		if ( ( sec == 0 ) && ( last_time_sec != 0 ) )
		{
			// Do periodic Stats update every minute
			sprintf(buf, "VS: Temp: %0.1f; RR: %d; awRR: %d; HR: %d; %s; BP: %d/%d; SPO2: %d; etCO2: %d mmHg; Probes: ECG: %s; BP: %s; SPO2: %s; ETCO2: %s; Temp %s",
				((double)simmgr_shm->status.general.temperature) / 10,
				simmgr_shm->status.respiration.rate,
				simmgr_shm->status.respiration.awRR,
				simmgr_shm->status.cardiac.rate,
				(simmgr_shm->status.cardiac.arrest == 1 ? "Arrest" : "Normal"  ),
				simmgr_shm->status.cardiac.bps_sys,
				simmgr_shm->status.cardiac.bps_dia,
				simmgr_shm->status.respiration.spo2,
				simmgr_shm->status.respiration.etco2,
				(simmgr_shm->status.cardiac.ecg_indicator == 1 ? "on" : "off"  ),
				(simmgr_shm->status.cardiac.bp_cuff == 1 ? "on" : "off"  ),
				(simmgr_shm->status.respiration.spo2_indicator == 1 ? "on" : "off"  ),
				(simmgr_shm->status.respiration.etco2_indicator == 1 ? "on" : "off"  ),
				(simmgr_shm->status.general.temperature_enable == 1 ? "on" : "off"  )
			);
			simlog_entry(buf );
		}
		last_time_sec = sec;
	}
	else if ( scenario_state == ScenarioStopped )
	{
		last_time_sec = -1;
	}
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

void
clearAllTrends(void )
{
	// Clear running trends
	(void)clearTrend(&cardiacTrend, simmgr_shm->status.cardiac.rate );
	(void)clearTrend(&sysTrend, simmgr_shm->status.cardiac.bps_sys  );
	(void)clearTrend(&diaTrend, simmgr_shm->status.cardiac.bps_dia  );
	(void)clearTrend(&respirationTrend, simmgr_shm->status.respiration.rate );
	(void)clearTrend(&spo2Trend, simmgr_shm->status.respiration.spo2 );
	(void)clearTrend(&etco2Trend, simmgr_shm->status.respiration.etco2 );
	(void)clearTrend(&tempTrend, simmgr_shm->status.general.temperature );
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

int
isRhythmPulsed(char *rhythm ) 
{
	if ( ( strcmp(rhythm, "asystole" ) == 0 ) ||
		 ( strcmp(rhythm, "vfib" ) == 0 ) )
	{
		return ( false );
	}
	else
	{
		return ( true );
	}
}

void
setRespirationPeriods(int oldRate, int newRate )
{
	int period;
	
	if ( oldRate != newRate )
	{
		if ( newRate > 0 )
		{
			simmgr_shm->status.respiration.rate = newRate;
			period = (1000*60)/newRate;	// Period in msec from rate per minute
			if ( period > 10000 )
			{
				period = 10000;
			}
			period = (period * 7) / 10;	// Use 70% of the period for duration calculations
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
}
	
/*
 * Scan commands from Initiator Interface
 *
 * Reads II commands and changes operating parameters
 *
 * Note: Events are added to the Event List directly by the source initiators and read
 * by the scenario process. Events are not handled here.
 */
int
scan_commands(void )
{
	int sts;
	int trycount;
	int oldRate;
	int newRate;
	int doRecord = -1;
	int currentIsPulsed;
	int newIsPulsed;
	// Lock the command interface before processing commands
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
	if ( simmgr_shm->instructor.scenario.record >= 0 )
	{
		doRecord = simmgr_shm->instructor.scenario.record;
		simmgr_shm->instructor.scenario.record = -1;
	}
	
	if ( strlen(simmgr_shm->instructor.scenario.state ) > 0 )
	{
		strToLower(simmgr_shm->instructor.scenario.state );
		
		sprintf(msgbuf, "State Request: %s Current %s State %d", 
			simmgr_shm->instructor.scenario.state,
			simmgr_shm->status.scenario.state,
			scenario_state );
		log_message("", msgbuf ); 
		
		if ( strcmp(simmgr_shm->instructor.scenario.state, "paused" ) == 0 )
		{
			if ( scenario_state == ScenarioRunning )
			{
				updateScenarioState(ScenarioPaused );
			}
		}
		else if ( strcmp(simmgr_shm->instructor.scenario.state, "running" ) == 0 )
		{
			if ( scenario_state == ScenarioPaused )
			{
				updateScenarioState(ScenarioRunning );
			}
			else if ( scenario_state == ScenarioStopped )
			{
				sts = start_scenario(simmgr_shm->status.scenario.active );
			}
		}
		else if ( strcmp(simmgr_shm->instructor.scenario.state, "terminate" ) == 0 )
		{
			if ( scenario_state != ScenarioTerminate )
			{
				updateScenarioState(ScenarioTerminate );
			}
		}
		else if ( strcmp(simmgr_shm->instructor.scenario.state, "stopped" ) == 0 )
		{
			if ( scenario_state != ScenarioStopped )
			{
				updateScenarioState(ScenarioStopped );
			}
		}
		sprintf(simmgr_shm->instructor.scenario.state, "%s", "" );
	}
	if ( strlen(simmgr_shm->instructor.scenario.active) > 0 )
	{
		sprintf(msgbuf, "Set Active: %s State %d", simmgr_shm->instructor.scenario.active, scenario_state );
		log_message("", msgbuf ); 
		switch ( scenario_state )
		{
			case ScenarioTerminate:
			default:
				break;
			case ScenarioStopped:
				sprintf(simmgr_shm->status.scenario.active, "%s", simmgr_shm->instructor.scenario.active );
				break;
		}
		sprintf(simmgr_shm->instructor.scenario.active, "%s", "" );
	}
	
	// Cardiac
	if ( strlen(simmgr_shm->instructor.cardiac.rhythm ) > 0 )
	{
		if ( strcmp(simmgr_shm->status.cardiac.rhythm, simmgr_shm->instructor.cardiac.rhythm ) != 0 )
		{
			// When changing to pulseless rhythm, the rate will be set to zero.
			newIsPulsed = isRhythmPulsed(simmgr_shm->instructor.cardiac.rhythm );
			if ( newIsPulsed == false )
			{
				simmgr_shm->instructor.cardiac.rate = 0;
				simmgr_shm->instructor.cardiac.transfer_time = 0;
			}
			else
			{
				// When changing from a pulseless rhythm to a pulse rhythm, the rate will be set to 100
				// This can be overridden in the command by setting the rate explicitly
				currentIsPulsed = isRhythmPulsed(simmgr_shm->status.cardiac.rhythm );
				if ( ( currentIsPulsed == false ) && ( newIsPulsed == true ) )
				{
					if ( simmgr_shm->instructor.cardiac.rate < 0 )
					{
						simmgr_shm->instructor.cardiac.rate = 100;
						simmgr_shm->instructor.cardiac.transfer_time = 0;
					}
				}
			}
			sprintf(simmgr_shm->status.cardiac.rhythm, "%s", simmgr_shm->instructor.cardiac.rhythm );
			sprintf(buf, "%s: %s", "Cardiac Rhythm", simmgr_shm->instructor.cardiac.rhythm );
			simlog_entry(buf );
		}
		sprintf(simmgr_shm->instructor.cardiac.rhythm, "%s", "" );
		
	}
	if ( simmgr_shm->instructor.cardiac.rate >= 0 )
	{
		currentIsPulsed = isRhythmPulsed(simmgr_shm->status.cardiac.rhythm );
		if ( currentIsPulsed == true )
		{
			if ( simmgr_shm->instructor.cardiac.rate != simmgr_shm->status.cardiac.rate )
			{
				simmgr_shm->status.cardiac.rate = setTrend(&cardiacTrend, 
												simmgr_shm->instructor.cardiac.rate,
												simmgr_shm->status.cardiac.rate,
												simmgr_shm->instructor.cardiac.transfer_time );
				if ( simmgr_shm->instructor.cardiac.transfer_time >= 0 )
				{
					sprintf(buf, "%s: %d time %d", "Cardiac Rate", simmgr_shm->instructor.cardiac.rate, simmgr_shm->instructor.cardiac.transfer_time );
				}
				else
				{
					sprintf(buf, "%s: %d", "Cardiac Rate", simmgr_shm->instructor.cardiac.rate );
				}
				simlog_entry(buf );
			}
		}
		else 
		{
			if ( simmgr_shm->instructor.cardiac.rate > 0 )
			{
				sprintf(buf, "%s: %d", "Cardiac Rate cannot be set while in pulseless rhythm", simmgr_shm->instructor.cardiac.rate );
				simlog_entry(buf );
			}
			else
			{
				simmgr_shm->status.cardiac.rate = setTrend(&cardiacTrend, 
												0,
												simmgr_shm->status.cardiac.rate,
												0 );
			}
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
	if ( simmgr_shm->instructor.cardiac.nibp_read >= 0 )
	{
		if ( simmgr_shm->status.cardiac.nibp_read != simmgr_shm->instructor.cardiac.nibp_read )
		{
			simmgr_shm->status.cardiac.nibp_read = simmgr_shm->instructor.cardiac.nibp_read;
		}
		simmgr_shm->instructor.cardiac.nibp_read = -1;
	}
	if ( simmgr_shm->instructor.cardiac.nibp_linked_hr >= 0 )
	{
		if ( simmgr_shm->status.cardiac.nibp_linked_hr != simmgr_shm->instructor.cardiac.nibp_linked_hr )
		{
			simmgr_shm->status.cardiac.nibp_linked_hr = simmgr_shm->instructor.cardiac.nibp_linked_hr;
		}
		simmgr_shm->instructor.cardiac.nibp_linked_hr = -1;
	}
	if ( simmgr_shm->instructor.cardiac.nibp_freq >= 0 )
	{
		if ( simmgr_shm->status.cardiac.nibp_freq != simmgr_shm->instructor.cardiac.nibp_freq )
		{
			simmgr_shm->status.cardiac.nibp_freq = simmgr_shm->instructor.cardiac.nibp_freq;
			if ( nibp_state == NibpWaiting ) // Cancel current wait and allow reset to new rate
			{
				nibp_state = NibpIdle;
			}
		}
		simmgr_shm->instructor.cardiac.nibp_freq = -1;
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
	if ( simmgr_shm->instructor.cardiac.right_dorsal_pulse_strength >= 0 )
	{
		simmgr_shm->status.cardiac.right_dorsal_pulse_strength = simmgr_shm->instructor.cardiac.right_dorsal_pulse_strength;
		simmgr_shm->instructor.cardiac.right_dorsal_pulse_strength = -1;
	}
	if ( simmgr_shm->instructor.cardiac.right_femoral_pulse_strength >= 0 )
	{
		simmgr_shm->status.cardiac.right_femoral_pulse_strength = simmgr_shm->instructor.cardiac.right_femoral_pulse_strength;
		simmgr_shm->instructor.cardiac.right_femoral_pulse_strength = -1;
	}
	if ( simmgr_shm->instructor.cardiac.left_dorsal_pulse_strength >= 0 )
	{
		simmgr_shm->status.cardiac.left_dorsal_pulse_strength = simmgr_shm->instructor.cardiac.left_dorsal_pulse_strength;
		simmgr_shm->instructor.cardiac.left_dorsal_pulse_strength = -1;
	}
	if ( simmgr_shm->instructor.cardiac.left_femoral_pulse_strength >= 0 )
	{
		simmgr_shm->status.cardiac.left_femoral_pulse_strength = simmgr_shm->instructor.cardiac.left_femoral_pulse_strength;
		simmgr_shm->instructor.cardiac.left_femoral_pulse_strength = -1;
	}
	if ( simmgr_shm->instructor.cardiac.vpc_freq >= 0 )
	{
		simmgr_shm->status.cardiac.vpc_freq = simmgr_shm->instructor.cardiac.vpc_freq;
		simmgr_shm->instructor.cardiac.vpc_freq = -1;
	}
	if ( simmgr_shm->instructor.cardiac.vpc_delay >= 0 )
	{
		simmgr_shm->status.cardiac.vpc_delay = simmgr_shm->instructor.cardiac.vpc_delay;
		simmgr_shm->instructor.cardiac.vpc_delay = -1;
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
	if ( simmgr_shm->instructor.cardiac.bp_cuff >= 0 )
	{
		if ( simmgr_shm->status.cardiac.bp_cuff != simmgr_shm->instructor.cardiac.bp_cuff )
		{
			simmgr_shm->status.cardiac.bp_cuff = simmgr_shm->instructor.cardiac.bp_cuff;
			sprintf(buf, "%s %s", "BP Cuff", (simmgr_shm->status.cardiac.bp_cuff == 1 ? "Attached": "Removed") );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.cardiac.bp_cuff = -1;
	}
	if ( simmgr_shm->instructor.cardiac.arrest >= 0 )
	{
		if ( simmgr_shm->status.cardiac.arrest != simmgr_shm->instructor.cardiac.arrest )
		{
			simmgr_shm->status.cardiac.arrest = simmgr_shm->instructor.cardiac.arrest;
			sprintf(buf, "%s %s", "Arrest", (simmgr_shm->status.cardiac.arrest == 1 ? "Start": "Stop") );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.cardiac.arrest = -1;
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
	/*
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
	*/
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
		sprintf(msgbuf, "Set Resp Rate = %d : %d", simmgr_shm->instructor.respiration.rate, simmgr_shm->instructor.respiration.transfer_time );
		log_message("", msgbuf);
		if ( simmgr_shm->instructor.respiration.transfer_time <= 0 )
		{
			setRespirationPeriods(simmgr_shm->status.respiration.rate,
								  simmgr_shm->instructor.respiration.rate );
		}
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
	if ( simmgr_shm->instructor.respiration.manual_breath >= 0 )
	{
		simmgr_shm->status.respiration.manual_count++;
		simmgr_shm->instructor.respiration.manual_breath = -1;
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
	if ( simmgr_shm->instructor.general.temperature_enable >= 0 )
	{
		if ( simmgr_shm->status.general.temperature_enable != simmgr_shm->instructor.general.temperature_enable )
		{
			simmgr_shm->status.general.temperature_enable = simmgr_shm->instructor.general.temperature_enable;
			sprintf(buf, "%s %s", "Temp Probe", (simmgr_shm->status.general.temperature_enable == 1 ? "Attached": "Removed") );
			simlog_entry(buf );
		}
		simmgr_shm->instructor.general.temperature_enable = -1;
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
	
	// media
	if ( strlen(simmgr_shm->instructor.media.filename) > 0 )
	{
		sprintf(simmgr_shm->status.media.filename, "%s", simmgr_shm->instructor.media.filename );
		sprintf(simmgr_shm->instructor.media.filename, "%s", "" );
	}
	if ( simmgr_shm->instructor.media.play != -1 )
	{
		simmgr_shm->status.media.play = simmgr_shm->instructor.media.play;
		simmgr_shm->instructor.media.play = -1;
	}
	
	// CPR
	if ( simmgr_shm->instructor.cpr.compression >= 0 )
	{
		simmgr_shm->status.cpr.compression = simmgr_shm->instructor.cpr.compression;
		simmgr_shm->instructor.cpr.compression = -1;
	}
	
	// Release the MUTEX
	sem_post(&simmgr_shm->instructor.sema);
	iiLockTaken = 0;
	
	// Process the trends
	// We do this even if no scenario is running, to allow an instructor simple, manual control
	simmgr_shm->status.cardiac.rate = trendProcess(&cardiacTrend );
	simmgr_shm->status.cardiac.bps_sys = trendProcess(&sysTrend );
	simmgr_shm->status.cardiac.bps_dia = trendProcess(&diaTrend );
	oldRate = simmgr_shm->status.respiration.rate;
	newRate = trendProcess(&respirationTrend );
	setRespirationPeriods(oldRate, newRate );

	// simmgr_shm->status.respiration.awRR = simmgr_shm->status.respiration.rate;
	simmgr_shm->status.respiration.spo2 = trendProcess( &spo2Trend );
	simmgr_shm->status.respiration.etco2 = trendProcess( &etco2Trend );
	simmgr_shm->status.general.temperature = trendProcess(&tempTrend );

	// NIBP processing
	now = std::time(nullptr );
	switch ( nibp_state )
	{
		case NibpIdle:	// Not started or BP Cuff detached
			if ( simmgr_shm->status.cardiac.bp_cuff > 0 )
			{
				if ( simmgr_shm->status.cardiac.nibp_read == 1 )
				{
					// Manual Start - Go to Running for the run delay time
					nibp_run_complete_time = now + NIBP_RUN_TIME;
					nibp_state = NibpRunning;
					sprintf(msgbuf, "NibpState Change: Idle to Running (%ld to %ld)", now, nibp_run_complete_time );
					log_message("", msgbuf ); 
				}
				else if ( simmgr_shm->status.cardiac.nibp_freq != 0 )
				{
					// Frequency set
					nibp_next_time = now + (simmgr_shm->status.cardiac.nibp_freq * 60);
					nibp_state = NibpWaiting;
					
					sprintf(msgbuf, "NibpState Change: Idle to Waiting" );
					log_message("", msgbuf ); 
				}
			}
			break;
		case NibpWaiting:
			if ( simmgr_shm->status.cardiac.bp_cuff == 0 ) // Cuff removed
			{
				nibp_state = NibpIdle;
			}
			else 
			{
				if ( simmgr_shm->status.cardiac.nibp_read == 1 )
				{
					// Manual Override
					nibp_next_time = now;
				}
				if ( nibp_next_time <= now )
				{
					nibp_run_complete_time = now + NIBP_RUN_TIME;
					nibp_state = NibpRunning;
					
					sprintf(msgbuf, "NibpState Change: Waiting to Running" );
					log_message("", msgbuf ); 
					simmgr_shm->status.cardiac.nibp_read = 1;
				}
			}
			break;
		case NibpRunning:
			if ( simmgr_shm->status.cardiac.bp_cuff == 0 ) // Cuff removed
			{
				nibp_state = NibpIdle;
				
				sprintf(msgbuf, "NibpState Change: Running to Idle (cuff removed)" );
				log_message("", msgbuf ); 
			}
			else 
			{
				if ( nibp_run_complete_time <= now )
				{
					simmgr_shm->status.cardiac.nibp_read = 0;
					if ( simmgr_shm->status.cardiac.nibp_freq != 0 )
					{
						// Frequency set
						nibp_next_time = now + (simmgr_shm->status.cardiac.nibp_freq * 60);
						nibp_state = NibpWaiting;
						
						sprintf(msgbuf, "NibpState Change: Running to Waiting" );
						log_message("", msgbuf ); 
					}
					else
					{
						nibp_state = NibpIdle;
						sprintf(msgbuf, "NibpState Change: Running to Idle" );
						log_message("", msgbuf ); 
					}
				}
			}
			break;
	}
	/*
		if the BP Cuff is attached and we see nibp_read set, then 
	*/
	if ( scenario_state == ScenarioTerminate )
	{
		if ( simmgr_shm->logfile.active == 0 )
		{
			updateScenarioState(ScenarioTerminate );
		}
	}
	else if ( scenario_state == ScenarioStopped )
	{
		if ( simmgr_shm->logfile.active == 0 )
		{
			updateScenarioState(ScenarioStopped );
		}
		if ( scenarioPid )
		{
			killScenario(1, NULL );
		}
	}
	// This must be done after the lock has been released as the recordStartStop process may block.
	if ( doRecord >= 0 )
	{
		recordStartStop(doRecord );
	}
	
	return ( 0 );
}

/**
 * recordStartStop
 * @record - Start if 1, Stop if 0
 *
 * Fork and then process start with external server
 */
 
void
recordStartStop(int record )
{
	//char fname[128];
	int pid;
	sprintf(msgbuf, "Start/Stop Record: %d", record );
	log_message("", msgbuf ); 
	
	pid = fork();
	
	if ( pid < 0 )
	{
		sprintf(msgbuf, "Start/Stop Record: Fork Failed %s", strerror(errno )  );
		log_message("", msgbuf ); 
	}
	else if ( pid == 0 )
	{
		// Child
		sprintf(msgbuf, "Start/Stop Record: %d  %p", record, simmgr_shm  );
		log_message("", msgbuf ); 
		// Sleep to simluate comm with server (until we get the server code functioning)
		sleep(7 );
		if ( record )
		{
			// Start Record
			simmgr_shm->status.scenario.record = 1;
		}
		else
		{
			// Stop Record
			simmgr_shm->status.scenario.record = 0;
		}
		// Stop the child
		exit ( 0 );
	}
	
}
int
start_scenario(const char *name )
{
	char timeBuf[64];
	char fname[128];

	sprintf(msgbuf, "Start Scenario Request: %s", name );
	log_message("", msgbuf ); 
	sprintf(fname, "%s/%s/main.xml", "/var/www/html/scenarios", name );
	
	scenario_start_time = std::time(nullptr );
	std::strftime(timeBuf, 60, "%c", std::localtime(&scenario_start_time ));
	
	resetAllParameters();
	
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
		
		updateScenarioState(ScenarioRunning );
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

/*
 * checkEvents
 * 
 * Scan through the event list and log any new events
 * Also scan comment list and and new ones to log file
 */

void
checkEvents(void )
{
	if ( ( lastEventLogged != simmgr_shm->eventListNext ) ||
		 ( lastCommentLogged != simmgr_shm->commentListNext ) )
	{
		takeInstructorLock();
		while ( lastEventLogged != simmgr_shm->eventListNext )
		{
			lastEventLogged++;
			if ( lastEventLogged >= EVENT_LIST_SIZE )
			{
				lastEventLogged = 0;
			}
			sprintf(msgbuf, "Event: %s", simmgr_shm->eventList[lastEventLogged].eventName );
			simlog_entry(msgbuf );
		}
		while ( lastCommentLogged != simmgr_shm->commentListNext )
		{
			lastCommentLogged++;
			if ( lastCommentLogged >= COMMENT_LIST_SIZE )
			{
				lastCommentLogged = 0;
			}
			if ( strlen(simmgr_shm->commentList[lastCommentLogged].comment ) == 0 )
			{
				sprintf(msgbuf, "Null Comment: lastCommentLogged is %d simmgr_shm->commentListNext is %d State is %d\n",
					lastCommentLogged, simmgr_shm->commentListNext, scenario_state );
				lastCommentLogged = simmgr_shm->commentListNext;
			}
			else
			{
				simlog_entry(simmgr_shm->commentList[lastCommentLogged].comment );
			}
		}
		releaseInstructorLock();
	}
}

void
resetAllParameters(void )
{
	// status/cardiac
	sprintf(simmgr_shm->status.cardiac.rhythm, "%s", "sinus" );
	sprintf(simmgr_shm->status.cardiac.vpc, "%s", "none" );
	sprintf(simmgr_shm->status.cardiac.vfib_amplitude, "%s", "high" );
	simmgr_shm->status.cardiac.vpc_freq = 10;
	simmgr_shm->status.cardiac.vpc_delay = 0;
	simmgr_shm->status.cardiac.pea = 0;
	simmgr_shm->status.cardiac.rate = 80;
	simmgr_shm->status.cardiac.nibp_rate = 80;
	simmgr_shm->status.cardiac.nibp_read = -1;
	simmgr_shm->status.cardiac.nibp_linked_hr = 1;
	simmgr_shm->status.cardiac.nibp_freq = 0;
	sprintf(simmgr_shm->status.cardiac.pwave, "%s", "none" );
	simmgr_shm->status.cardiac.pr_interval = 140; // Good definition at http://lifeinthefastlane.com/ecg-library/basics/pr-interval/
	simmgr_shm->status.cardiac.qrs_interval = 85;
	simmgr_shm->status.cardiac.bps_sys = 105;
	simmgr_shm->status.cardiac.bps_dia = 70;
	simmgr_shm->status.cardiac.right_dorsal_pulse_strength = 2;
	simmgr_shm->status.cardiac.right_femoral_pulse_strength = 2;
	simmgr_shm->status.cardiac.left_dorsal_pulse_strength = 2;
	simmgr_shm->status.cardiac.left_femoral_pulse_strength = 2;
	sprintf(simmgr_shm->status.cardiac.heart_sound, "%s", "none" );
	simmgr_shm->status.cardiac.heart_sound_volume = 10;
	simmgr_shm->status.cardiac.heart_sound_mute = 0;
	simmgr_shm->status.cardiac.ecg_indicator = 0;
	simmgr_shm->status.cardiac.bp_cuff = 0;
	simmgr_shm->status.cardiac.arrest = 0;
	
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
	simmgr_shm->status.respiration.manual_breath = 0;
	simmgr_shm->status.respiration.manual_count = 0;
	
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
	simmgr_shm->status.pulse.right_dorsal = 0;
	simmgr_shm->status.pulse.left_dorsal = 0;
	simmgr_shm->status.pulse.right_femoral = 0;
	simmgr_shm->status.pulse.left_femoral = 0;
	
	// status/cpr
	simmgr_shm->status.cpr.last = 0;
	simmgr_shm->status.cpr.compression = 0;
	simmgr_shm->status.cpr.release = 0;
	simmgr_shm->status.cpr.duration = 0;
	
	// status/defibrillation
	simmgr_shm->status.defibrillation.last = 0;
	simmgr_shm->status.defibrillation.energy = 0;
	
	// status/general
	simmgr_shm->status.general.temperature = 1017;
	simmgr_shm->status.general.temperature_enable = 0;
	
	// status/media
	sprintf(simmgr_shm->status.media.filename, "%s", "" );
	simmgr_shm->status.media.play = 0;
		
	// instructor/cardiac
	sprintf(simmgr_shm->instructor.cardiac.rhythm, "%s", "" );
	simmgr_shm->instructor.cardiac.rate = -1;
	simmgr_shm->instructor.cardiac.nibp_rate = -1;
	simmgr_shm->instructor.cardiac.nibp_read = -1;
	simmgr_shm->instructor.cardiac.nibp_linked_hr = -1;
	simmgr_shm->instructor.cardiac.nibp_freq = -1;
	sprintf(simmgr_shm->instructor.cardiac.pwave, "%s", "" );
	simmgr_shm->instructor.cardiac.pr_interval = -1;
	simmgr_shm->instructor.cardiac.qrs_interval = -1;
	simmgr_shm->instructor.cardiac.bps_sys = -1;
	simmgr_shm->instructor.cardiac.bps_dia = -1;
	simmgr_shm->instructor.cardiac.pea = -1;
	simmgr_shm->instructor.cardiac.vpc_freq = -1;
	simmgr_shm->instructor.cardiac.vpc_delay = -1;
	sprintf(simmgr_shm->instructor.cardiac.vpc, "%s", "" );
	sprintf(simmgr_shm->instructor.cardiac.vfib_amplitude, "%s", "" );
	simmgr_shm->instructor.cardiac.right_dorsal_pulse_strength = -1;
	simmgr_shm->instructor.cardiac.right_femoral_pulse_strength = -1;
	simmgr_shm->instructor.cardiac.left_dorsal_pulse_strength = -1;
	simmgr_shm->instructor.cardiac.left_femoral_pulse_strength = -1;
	sprintf(simmgr_shm->instructor.cardiac.heart_sound, "%s", "" );
	simmgr_shm->instructor.cardiac.heart_sound_volume = -1;
	simmgr_shm->instructor.cardiac.heart_sound_mute = -1;
	simmgr_shm->instructor.cardiac.ecg_indicator = -1;
	simmgr_shm->instructor.cardiac.bp_cuff = -1;
	simmgr_shm->instructor.cardiac.arrest = -1;
	
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
	simmgr_shm->instructor.respiration.manual_breath = -1;
	simmgr_shm->instructor.respiration.manual_count = -1;
	
	// instructor/media
	sprintf(simmgr_shm->instructor.media.filename, "%s", "" );
	simmgr_shm->instructor.media.play = -1;
	
	// instructor/general
	simmgr_shm->instructor.general.temperature = -1;
	simmgr_shm->instructor.general.temperature_enable = -1;

	// instructor/vocals
	sprintf(simmgr_shm->instructor.vocals.filename, "%s", "" );
	simmgr_shm->instructor.vocals.repeat = -1;
	simmgr_shm->instructor.vocals.volume = -1;
	simmgr_shm->instructor.vocals.play = -1;
	simmgr_shm->instructor.vocals.mute = -1;
	
	clearAllTrends();
}