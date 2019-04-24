/*
 * scenario.cpp
 *
 * Scenario Processing
 *
 * This file is part of the sim-mgr distribution (https://github.com/OpenVetSimDevelopers/sim-mgr).
 * 
 * Copyright (c) 2019 VetSim, Cornell University College of Veterinary Medicine Ithaca, NY
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * The Scenario runs as an independent process. It is exec'ed from the simmgr and acts
 * using the shared memory space to monitor the system and inject Initiator commands
 * as controlled by the scenario script.
 *
 * The scenario script is an XML formatted file. It is parsed by the scenario process to
 * define the various scenes. A scene contains a set of initialization parameters and
 * a set a triggers.
 *
 * The scenario process runs as a child process of the simmgr daemon. It is started by
 * the simmgr on command from the Instructor Interface and can be terminated on command
 * as well. 
 *
 * On completion of the scenario, the scenario process is expected to enter a "Ending"
 * scene. More than one ending scene may exist, allow outcomes with either a healthy
 * or expired patient, or other states of health as well.
 *
 * Definitions:
 *
 * Init: A list of parameter definitions, with or without trends. The init parameters are
 *       applied at the entry to a scene.
 *       A global init is also defined. This is applied at the entry to the scenario.
 *
 *    Vocals in Init:
 * 		Vocals may be set in an init definition. This may be used to invoke a vocalization
 *      at the beginning of scene. The vocalization is invoked immediately when parsed. 
 *
 * Scene: A state definition within the scenario. 
 *
 * Trigger: A criteria for termination of a scene. The trigger defines the parameter to be
 *          watched, the threshold for firing and the next scene to enter.
 *
 * Ending Scene: A scene that ends the scenario. This scene has an init, but no triggers. 
 *          It also contains an <end> directive, which will cause the scenario process
 *          to end.
 *
 * Process End:
 *		On completion, the scenario process will print a single line and then exit. If the
 *      end is due to entry of an ending scene, the printed line is the content from the
 *		<end> directive. If exit is due to an error, the line will describe the error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>


#include <libxml2/libxml/xmlreader.h>
#include <libxml2/libxml/tree.h>
#include <libxml2/libxml/parser.h>
#include <libxml2/libxml/xpath.h>
#include <libxml2/libxml/xpathInternals.h>

#include "../include/scenario.h"

#ifndef LIBXML_READER_ENABLED
	Error LIBXML is not included or Reader is not Enabled
#endif

struct xml_level xmlLevels[10];
int xml_current_level = 0;
int line_number = 0;
const char *xml_filename;
int verbose = 0;
int checkOnly = 0;
int runningAsDemo = 0;
int errCount = 0;

// Internal state is tracked to compare to the overall state, for detecting changes
ScenarioState scenario_state;

char current_event_catagory[NORMAL_STRING_SIZE];
char current_event_title[NORMAL_STRING_SIZE];

int current_scene_id = -1;
struct scenario_scene *current_scene;

struct scenario_data *scenario;
struct scenario_scene *new_scene;
struct scenario_trigger *new_trigger;
struct scenario_event *new_event;

int parse_state = PARSE_STATE_NONE;
int parse_init_state = PARSE_INIT_STATE_NONE;
int parse_scene_state = PARSE_SCENE_STATE_NONE;
int parse_header_state = PARSE_HEADER_STATE_NONE;

static void saveData(const xmlChar *xmlName, const xmlChar *xmlValue );
static int readScenario(const char *filename);
static void scene_check(void );
static struct scenario_scene *findScene(int scene_id );
static struct scenario_scene *showScenes();
static void startScene(int sceneId );

// loopStart and loopStop are used to measure the actual sleep time of the scenario loop,
// to calculate the time in a scene and in the scenario
struct timespec loopStart;
struct timespec loopStop;

int eventLast;	// Index of last processed event_callback

struct timespec cprStart; // Time of first CPR detected
int cprActive = 0;				// Flag to indicate CPR is active
int cprCumulative = 0;		// Cumulative time for CPR active in this scene
int shockActive = 0;	// Flag to indicate Defibrillation is active
struct pulse puseStatus = { 0, 0, 0, 0 };

const char *parse_states[] =
{
	"PARSE_STATE_NONE", "PARSE_STATE_INIT", "PARSE_STATE_SCENE",
	"PARSE_STATE_HEADER"
};

const char *parse_init_states[] =
{
	"NONE", "CARDIAC", "RESPIRATION",
	"GENERAL", "SCENE", "VOCALS"
};

const char *parse_scene_states[] =
{
	"NONE", "INIT", "CARDIAC", "RESPIRATION",
	"GENERAL", "VOCALS", "TIMEOUT", "TRIGS", "TRIG"
};

const char *parse_header_states[] =
{
	"NONE", "AUTHOR", "TITLE", "DATE_OF_CREATION",
	"DESCRIPTION"
};

const char *trigger_tests[] =
{
	"EQ", "LTE", "LT", "GTE", "GT", "INSIDE", "OUTSIDE", "EVENT"
};

const char *trigger_tests_sym[] =
{
	"==", "<=", "<", ">=", ">", "", "", ""
};

char logMsg[512];
/**
 * main:
 * @argc: number of argument
 * @argv: pointer to argument array
 *
 * Returns:
 *		Exit val of 0 is successful completion.
 *		Any other exit is failure.
 */

char usage[] = "[-cv] [-S sessionID] xml-file-name";
#define MAX_MSGBUF_SIZE 1024
char msgbuf[MAX_MSGBUF_SIZE];
char getArgList[] = "cvS:q";

int 
main(int argc, char **argv)
{
	int loopCount;
	int c;
	int sts;
	char *sesid = NULL;
	int q = false;
	int i;
	
	opterr = 0;
	while ((c = getopt(argc, argv, getArgList )) != -1 )
	{
		switch (c)
		{
			case 'c':
				checkOnly = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'S':
				sesid = optarg;
				runningAsDemo = 1;
				break;
			case 'q':
				q = true;
				break;
			case '?':
				if (isprint (optopt))
					fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				else
					fprintf (stderr,
						   "Unknown option character `\\x%x'.\n",
						   optopt);
				return 1;
			default:
				abort();
		}
		
	}
	
	if ( !q && optind >= argc )
	{
		//sprintf(msgbuf, "%s: %s\n", argv[0], usage );
		//log_message("", msgbuf );
		printf("%s: %s\n", argv[0], usage );
        return(1);
	}
	
	
		
	if ( checkOnly )
	{
		// For check only, fake the share memory space
		simmgr_shm = (struct simmgr_shm *)calloc(1, sizeof(struct simmgr_shm ) );
	}
	else
	{
		// Wait for Shared Memory to become available
		while ( initSHM(OPEN_ACCESS, sesid ) != 0 )
		{
			snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: SHM Failed - waiting" );
			log_message("", msgbuf );
			sleep(10 );
		}
	}
	if ( q ) 
	{
		// Show Comment data
		printf("commentListNext: %d\n", simmgr_shm->commentListNext );
		for ( i = 0 ; i < COMMENT_LIST_SIZE ; i++ )
		{
			printf("%d: %s\n", i, simmgr_shm->commentList[i].comment );
		}
		exit ( 0 );
	}
	
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scenario File %s SessionID %s", argv[optind], sesid );
	if ( !checkOnly )
	{
		log_message("", msgbuf );
	}
	if ( verbose )
	{
		fprintf(stderr, "%s\n", msgbuf );
	}
	
	simmgr_shm->status.scenario.elapsed_msec_scenario = 0;
	simmgr_shm->status.scenario.elapsed_msec_scene = 0;
	
	// Allocate and clear the base scenario structure
	scenario = (struct scenario_data *)calloc(1, sizeof(struct scenario_data) );
	initializeParameterStruct(&scenario->initParams );
	
	eventLast = simmgr_shm->eventListNext;	// Start processing event at the next posted event
	
    // Initialize the library and check potential ABI mismatches 
    LIBXML_TEST_VERSION

    if ( readScenario(argv[optind]) < 0 )
	{
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: readScenario Fails" );
		if ( !checkOnly )
		{
			log_message("", msgbuf );
		}
		if ( verbose )
		{
			fprintf(stderr, "%s\n", msgbuf );
		}
		return ( -1 );
	}
	
    xmlCleanupParser();
	
    // this is to debug memory for regression tests
    xmlMemoryDump();
	
	if ( verbose || checkOnly )
	{
		printf("Showing scenes\n" );
		showScenes();
	}
	if ( verbose )
	{
		printf("Calling findScene for scenario\n" );
	}
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: Calling findScene for scenario" );
	if ( !checkOnly )
	{
		log_message("", msgbuf );
	}
	if ( verbose )
	{
		fprintf(stderr, "%s\n", msgbuf );
	}
	
	// Get the name of the current scene
	current_scene = findScene(current_scene_id );
	if ( ! current_scene )
	{
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Starting scene not found in XML file" );
		current_scene_id = -1;
		if ( !checkOnly )
		{
			lockAndComment(msgbuf );
		}
		if ( verbose )
		{
			fprintf(stderr, "%s\n", msgbuf );
		}
		if ( ! checkOnly )
		{
			sprintf(simmgr_shm->status.scenario.scene_name, "%s", "No Start Scene" );
			takeInstructorLock();
			sprintf(simmgr_shm->instructor.scenario.state, "%s", "terminate" );
			releaseInstructorLock();
		}
		errCount++;
	}
	else
	{
		if ( ! checkOnly )
		{
			sprintf(simmgr_shm->status.scenario.scene_name, "%s", current_scene->name );
		}
	}
	
	if ( checkOnly )
	{
		return ( errCount );
	}
	
	if ( verbose )
	{
		printf("Calling processInit for scenario\n" );
	}
	///snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: Calling processInit for scenario" );
	//log_message("", msgbuf );
	simmgr_shm->status.cpr.compression = 0;
	simmgr_shm->status.cpr.duration = 0;
	simmgr_shm->status.defibrillation.energy = 100;
	simmgr_shm->status.cardiac.bp_cuff = 0;
	simmgr_shm->status.cardiac.ecg_indicator = 0;
	simmgr_shm->status.cardiac.pea = 0;
	simmgr_shm->status.cardiac.arrest = 0;
	simmgr_shm->status.respiration.etco2_indicator = 0;
	simmgr_shm->status.respiration.spo2_indicator = 0;
	simmgr_shm->status.respiration.chest_movement = 0;
	simmgr_shm->status.respiration.manual_breath = 0;
	simmgr_shm->status.respiration.manual_count = 0;
	simmgr_shm->status.general.temperature_enable = 0;
	
	// Log the Scenario Name
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "Title: %s", scenario->title );
	lockAndComment(msgbuf );
	/*
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "Author: %s", scenario->author );
	lockAndComment(msgbuf );
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "Created: %s", scenario->date_created );
	lockAndComment(msgbuf );
	snprintf(msgbuf, MAX_MSGBUF_SIZE, "Description: %s", scenario->description );
	lockAndComment(msgbuf );
	*/
	
	// Apply initialization parameters
	processInit(&scenario->initParams );
	
	if ( current_scene_id >= 0 ) 
	{
		if ( verbose )
		{
			printf("Calling processInit for Scene %d, %s \n", current_scene->id, current_scene->name );
		}
		startScene(current_scene_id );
	}

	// Set our internal state to running
	scenario_state = ScenarioRunning;
	
	if ( verbose )
	{
		printf("Starting Loop\n" );
	}

	simmgr_shm->status.scenario.elapsed_msec_scenario = 0;
	simmgr_shm->status.scenario.elapsed_msec_scene = 0;
	
	cprActive = 0;
	cprCumulative = 0;
	shockActive = 0;
	
	// Continue scenario execution
	loopCount = 0;
	while ( 1 )
	{
		clock_gettime( CLOCK_REALTIME, &loopStart );
		
		// Sleep
		usleep(250000 );	// Roughly a quarter second. usleep is not very accurate.
		
		if ( strcmp(simmgr_shm->status.scenario.state, "Terminate" ) == 0 )	// Check for termination
		{
			if ( scenario_state != ScenarioTerminate )
			{
				// If the scenario needs to do any cleanup, this is the place.
				
				// After setting state to Stopped, the simmgr will kill the scenario process
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: Terminate" );
				//log_message("", msgbuf );
				
				sts = takeInstructorLock();
				if ( !sts )
				{
					addComment(msgbuf );
					scenario_state = ScenarioTerminate;
					sprintf(simmgr_shm->instructor.scenario.state, "Stopped" );
					releaseInstructorLock();
				}
			}
		}
		else if ( strcmp(simmgr_shm->status.scenario.state, "Stopped" ) == 0 )
		{
			if ( scenario_state != ScenarioStopped )
			{
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "scenario: Stopped" );
				//log_message("", msgbuf );
				lockAndComment(msgbuf );
				scenario_state = ScenarioStopped;
			}
		}
		else if ( strcmp(simmgr_shm->status.scenario.state, "Running" ) == 0 )
		{
			// Do periodic scenario check
			scene_check();
			scenario_state = ScenarioRunning;
		}
		else if ( strcmp(simmgr_shm->status.scenario.state, "Paused" ) == 0 )
		{
			// Nothing
			scenario_state = ScenarioPaused;
		}
		if ( loopCount++ == 100 )
		{
			/*sprintf(msgbuf, "%s: timeout %d elapsed %d %d %d HR %d  BP %d/%d",
				simmgr_shm->status.scenario.state, current_scene->timeout, 
				simmgr_shm->status.scenario.elapsed_msec_absolute, simmgr_shm->status.scenario.elapsed_msec_scenario, simmgr_shm->status.scenario.elapsed_msec_scene,
				getValueFromName((char *)"cardiac", (char *)"rate"),
				getValueFromName((char *)"cardiac", (char *)"bps_sys"),
				getValueFromName((char *)"cardiac", (char *)"bps_dia") );
			log_message("", msgbuf ); */
			loopCount = 0;
		}
	}
	
    return(0);
}

/**
 * findScene
 * @scene_id
 *
*/
static struct scenario_scene *
findScene(int scene_id )
{
	struct snode *snode;
	struct scenario_scene *scene;
	//int limit = 50;
	
	snode = scenario->scene_list.next;
	
	while ( snode )
	{
		scene = (struct scenario_scene *)snode;
		if ( scene->id == scene_id )
		{
			return ( scene );
		}
		/*
		if ( limit-- == 0 )
		{
			printf("findScene Limit reached\n" );
			sprintf(msgbuf, "scenario: findScene Limit reached" );
			log_message("", msgbuf );
			lockAndComment(msgbuf );
			exit ( -2 );
		}
		*/
		snode = get_next_llist(snode );
	}
	return ( NULL );
}

/**
 *  scanForDuplicateScene
 * @scene_id
 *
 * Returns the number of matches found.
*/

int
scanForDuplicateScene(int sceneId )
{
	struct snode *snode;
	struct scenario_scene *scene;
	int match = 0;
	
	snode = scenario->scene_list.next;
	
	while ( snode )
	{
		scene = (struct scenario_scene *)snode;
		if ( scene->id == sceneId )
		{
			printf("sfds: %d %d\n", scene->id, sceneId );
			match++;
		}
		snode = get_next_llist(snode );
	}
	if ( match < 1 )
	{
		printf("ERROR: Scene ID %d not found\n", sceneId );
		errCount++;
	}
	else if ( match > 1 )
	{
		printf("ERROR: duplicate check, Scene ID %d found %d times\n", sceneId, match );
		errCount++;
	}
	return ( match );
}
/**
 *  scanForDuplicateEvent
 * @scene_id
 *
 * Returns the number of matches found.
*/

int
scanForDuplicateEvent(char *eventId )
{
	struct snode *snode;
	struct scenario_event *event;
	int match = 0;
	
	snode = scenario->event_list.next;
	
	while ( snode )
	{
		event = (struct scenario_event *)snode;
		if ( strcmp(event->event_id, eventId ) == 0 )
		{
			match++;
		}
		snode = get_next_llist(snode );
	}
	if ( match < 1 )
	{
		printf("ERROR: Event ID %s not found\n", eventId );
		errCount++;
	}
	else if ( match > 1 )
	{
		printf("ERROR: duplicate check, Event ID %s found %d times\n", eventId, match );
		errCount++;
	}
	return ( match );
}
/**
 *  showScenes
 * @scene_id
 *
*/
static struct scenario_scene *
showScenes()
{
	struct snode *snode;
	struct scenario_scene *scene;
	struct scenario_trigger *trig;
	struct scenario_event *event;
	struct snode *t_snode;
	struct snode *e_snode;
	int duplicates;
	int tcount;
	int timeout = 0;
	
	snode = scenario->scene_list.next;
	
	while ( snode )
	{
		scene = (struct scenario_scene *)snode;
		printf("Scene %d: %s\n", scene->id, scene->name );
		if ( scene->id < 0 )
		{
			printf("ERROR: Scene ID %d is invalid\n",
				scene->id );
			errCount++;
		}
		duplicates = scanForDuplicateScene(scene->id );
		if ( duplicates != 1 )
		{
			printf("ERROR: Scene ID %d has %d entries\n",
				scene->id, duplicates );
			errCount++;
		}
		tcount = 0;
		timeout = 0;
		if ( scene->timeout > 0 )
		{
			printf("\tTimeout: %d Scene %d\n", scene->timeout, scene->timeout_scene );
			timeout++;
		}
		printf("\tTriggers:\n" );
		t_snode = scene->trigger_list.next;
		while ( t_snode )
		{
			tcount++;
			trig = (struct scenario_trigger *)t_snode;
			if ( trig->test == TRIGGER_TEST_EVENT )
			{
				printf("\t\tEvent : %s\n", trig->param_element );
			}
			else
			{
				printf("\t\t%s:%s, %s, %d, %d - Scene %d\n", 
					trig->param_class, trig->param_element, trigger_tests[trig->test], trig->value, trig->value2, trig->scene );
			}
			t_snode = get_next_llist(t_snode );
		}
		if ( ( tcount == 0 ) && ( timeout == 0 ) && ( scene->id != 0 ) )
		{
			printf("ERROR: Scene ID %d has no trigger/timeout events\n",
				scene->id );
			errCount++;
		}
		if ( ( scene->id == 0 ) && ( tcount != 0 ) && ( timeout != 0 ) )
		{
			printf("ERROR: End Scene ID %d has %d triggers %d timeouts. Should be none.\n",
				scene->id, tcount, timeout );
			errCount++;
		}
		snode = get_next_llist(snode );
	}
	printf("Events:\n" );
	e_snode = scenario->event_list.next;
	while ( e_snode )
	{
		event = (struct scenario_event *)e_snode;
		printf("\t'%s'\t'%s'\t'%s'\t'%s'\n", 
			event->event_catagory_name, event->event_catagory_title, event->event_title, event->event_id );
		duplicates = scanForDuplicateEvent(event->event_id );
		if ( duplicates != 1 )
		{
			printf("ERROR: Event ID %s has %d entries\n",
				event->event_id, duplicates );
			errCount++;
		}
		e_snode = get_next_llist(e_snode );
	}
	return ( NULL );
}

void
logTrigger(struct scenario_trigger *trig, int time )
{
	if ( trig )
	{
		switch ( trig->test )
		{
			case TRIGGER_TEST_EVENT:
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger Event: %s", trig->param_element );
				break;
		
			case TRIGGER_TEST_INSIDE:
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger: %d < %s:%s < %d", 
					trig->value, trig->param_class, trig->param_element, trig->value2 );
				break;
		
			case TRIGGER_TEST_OUTSIDE:
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger: %d > %s:%s > %d", 
					trig->value, trig->param_class, trig->param_element, trig->value2 );
				break;
				
			default:
				snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger: %s:%s %s %d", 
					trig->param_class, trig->param_element, trigger_tests_sym[trig->test], trig->value );
				break;
		}
	}
	else if ( time )
	{
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger: Timeout %d seconds", time );
	}
	else
	{
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene Trigger: unknown" );
	}
		
	lockAndComment(msgbuf );
}

/**
* pulse_check
*
* Check for start/stop palpations
*/
static void pulse_check(void )
{
	if ( ! puseStatus.right_dorsal && simmgr_shm->status.pulse.right_dorsal )
	{
		puseStatus.right_dorsal = true;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Start Pulse Palpation Right Dorsal " );
		lockAndComment(msgbuf);
	}
	else if ( puseStatus.right_dorsal && ! simmgr_shm->status.pulse.right_dorsal )
	{
		puseStatus.right_dorsal = false;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "End Pulse Palpation Right Dorsal " );
		lockAndComment(msgbuf);
	}
	
	if ( ! puseStatus.left_dorsal && simmgr_shm->status.pulse.left_dorsal )
	{
		puseStatus.left_dorsal = true;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Start Pulse Palpation Left Dorsal " );
		lockAndComment(msgbuf);
	}
	else if ( puseStatus.left_dorsal && ! simmgr_shm->status.pulse.left_dorsal )
	{
		puseStatus.left_dorsal = false;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "End Pulse Palpation Left Dorsal " );
		lockAndComment(msgbuf);
	}
	
	if ( ! puseStatus.right_femoral && simmgr_shm->status.pulse.right_femoral )
	{
		puseStatus.right_femoral = true;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Start Pulse Palpation Right Femoral " );
		lockAndComment(msgbuf);
	}
	else if ( puseStatus.right_femoral && ! simmgr_shm->status.pulse.right_femoral )
	{
		puseStatus.right_femoral = false;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "End Pulse Palpation Right Femoral " );
		lockAndComment(msgbuf);
	}
	
	if ( ! puseStatus.left_femoral && simmgr_shm->status.pulse.left_femoral )
	{
		puseStatus.left_femoral = true;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Start Pulse Palpation Left Femoral " );
		lockAndComment(msgbuf);
	}
	else if ( puseStatus.left_femoral && ! simmgr_shm->status.pulse.left_femoral )
	{
		puseStatus.left_femoral = false;
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "End Pulse Palpation Left Femoral " );
		lockAndComment(msgbuf);
	}
}
/**
* scene_check
*
* Scan through the events and triggers and change scene if needed
*/

static void
scene_check(void )
{
	struct scenario_trigger *trig;
	struct snode *snode;
	int val;
	int met = 0;
	int msec_diff;
	int sec_diff;
	
	// Event checks 
	while ( simmgr_shm->eventListNext != eventLast )
	{
		snode = current_scene->trigger_list.next;
		while ( snode )
		{
			trig = (struct scenario_trigger *)snode;
			if ( trig->test == TRIGGER_TEST_EVENT )
			{
				if ( strcmp(trig->param_element, simmgr_shm->eventList[eventLast].eventName ) == 0 )
				{
					eventLast++;
					logTrigger(trig, 0 );
					startScene(trig->scene );
					return;
				}
			}
			snode = get_next_llist(snode );
		}
		eventLast++;
		if ( eventLast >= EVENT_LIST_SIZE )
		{
			eventLast = 0;
		}
	}
	if ( shockActive )
	{
	}
	if ( cprActive )
	{
		if ( simmgr_shm->status.cpr.compression == 0 )
		{
			cprActive = 0;
			snprintf(msgbuf, MAX_MSGBUF_SIZE, "Stopping Compressions: Cumulative %d seconds", cprCumulative );
			lockAndComment(msgbuf );
		}
		else
		{
			clock_gettime( CLOCK_REALTIME, &loopStop );
			cprCumulative += ( loopStop.tv_sec - cprStart.tv_sec );
			clock_gettime( CLOCK_REALTIME, &cprStart );
			simmgr_shm->status.cpr.duration = cprCumulative;
		}
	}
	else
	{
		if ( simmgr_shm->status.cpr.compression )
		{
			clock_gettime( CLOCK_REALTIME, &cprStart );
			cprActive = 1;
			snprintf(msgbuf, MAX_MSGBUF_SIZE, "Starting Compressions" );
			lockAndComment(msgbuf );
		}
	}
	
	// Pulse Palpation Checks
	pulse_check();
	
	// Trigger Checks
	snode = current_scene->trigger_list.next;
	while ( snode )
	{
		trig = (struct scenario_trigger *)snode;
		
		val = getValueFromName(trig->param_class, trig->param_element );
		switch ( trig->test )
		{
			case TRIGGER_TEST_EQ:
				if ( val == trig->value )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_LTE:
				if ( val <= trig->value )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_LT:
				if ( val < trig->value )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_GTE:
				if ( val >= trig->value )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_GT:
				if ( val > trig->value )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_INSIDE:
				if ( ( val > trig->value ) && ( val < trig->value2 ) )
				{
					met = 1;
				}
				break;
			case TRIGGER_TEST_OUTSIDE:
				if ( ( val < trig->value ) || ( val > trig->value2 ) )
				{
					met = 1;
				}
				break;	
		}

		if ( met )
		{
			logTrigger(trig, 0 );
			startScene(trig->scene );
			return;
		}
		
		snode = get_next_llist(snode );
	}
	// Check timeout
	clock_gettime( CLOCK_REALTIME, &loopStop );
	sec_diff = ( loopStop.tv_sec - loopStart.tv_sec );
	msec_diff = ( ( ( sec_diff * 1000000000)+loopStop.tv_nsec) - loopStart.tv_nsec ) / 1000000;
	simmgr_shm->status.scenario.elapsed_msec_scenario += msec_diff;
	simmgr_shm->status.scenario.elapsed_msec_scene += msec_diff;
		
	if ( current_scene->timeout )
	{
		if ( simmgr_shm->status.scenario.elapsed_msec_scene >=  ( current_scene->timeout * 1000 ) )
		{
			logTrigger((struct scenario_trigger *)0, current_scene->timeout );
			startScene(current_scene->timeout_scene );
		}
	}
}
/** startScene
 * @sceneId: id of new scene
 *
*/
static void
startScene(int sceneId )
{
	struct scenario_scene *new_scene;
	new_scene = findScene(sceneId );
	if ( ! new_scene )
	{
		fprintf(stderr, "Scene %d not found", sceneId );
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Scene %d not found. Terminating.", sceneId );
		takeInstructorLock();
		addComment(msgbuf );
		sprintf(simmgr_shm->instructor.scenario.state, "%s", "Terminate" );
		sprintf(simmgr_shm->status.scenario.scene_name, "%s", "" );
		releaseInstructorLock();
		return;
	}
	current_scene = new_scene;
	simmgr_shm->status.scenario.elapsed_msec_scene = 0;
	cprCumulative = 0;
	sprintf(simmgr_shm->status.scenario.scene_name, "%s", current_scene->name );

	simmgr_shm->status.scenario.scene_id = sceneId;
	simmgr_shm->status.respiration.manual_count = 0;
	
	if ( current_scene->id <= 0 )
	{
		if ( verbose )
		{
			printf("End scene %s\n", current_scene->name );
		}
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "End Scene %d %s", sceneId, current_scene->name );
		takeInstructorLock();
		addComment(msgbuf );
		sprintf(simmgr_shm->instructor.scenario.state, "%s", "Terminate" );
		releaseInstructorLock();
	}
	else
	{
		if ( verbose )
		{
			printf("New scene %s\n", current_scene->name );
		}
		snprintf(msgbuf, MAX_MSGBUF_SIZE, "Start Scene %d: %s", sceneId, current_scene->name );
		lockAndComment(msgbuf );
		processInit(&current_scene->initParams );
	}
}
/**
 * saveData:
 * @xmlName: name of the entry
 * @xmlValue: Text data to convert and save in structure
 *
 * Take the current Text entry and save as appropriate in the currently
 * named data structure
*/
static void
saveData(const xmlChar *xmlName, const xmlChar *xmlValue )
{
	char *name = (char *)xmlName;
	char *value = (char *)xmlValue;
	int sts = 0;
	int i;
	char *value2;
	
	switch ( parse_state )
	{
		case PARSE_STATE_NONE:
			if ( verbose )
			{
				printf("STATE_NONE: Lvl %d Name %s, Value, %s\n",
							xml_current_level, xmlLevels[xml_current_level].name, value );
			}
			break;
		case PARSE_STATE_INIT:
			switch ( parse_init_state )
			{
				case PARSE_INIT_STATE_NONE:
					if ( verbose )
					{
						printf("INIT_NONE: Lvl %d Name %s, Value, %s\n",
							xml_current_level, xmlLevels[xml_current_level].name, value );
					}
					sts = 0;
					break;
				
				case PARSE_INIT_STATE_CARDIAC:
					if ( xml_current_level == 3 )
					{
						sts = cardiac_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.cardiac );
					}
					break;
				case PARSE_INIT_STATE_RESPIRATION:
					if ( xml_current_level == 3 )
					{
						sts = respiration_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.respiration );
					}
					break;
				case PARSE_INIT_STATE_GENERAL:
					if ( xml_current_level == 3 )
					{
						sts = general_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.general );
					}
					break;
				case PARSE_INIT_STATE_VOCALS:
					if ( xml_current_level == 3 )
					{
						sts = vocals_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.vocals );
					}
					break;
				case PARSE_INIT_STATE_MEDIA:
					if ( xml_current_level == 3 )
					{
						sts = media_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.media );
					}
					break;
				case PARSE_INIT_STATE_CPR:
					if ( xml_current_level == 3 )
					{
						sts = cpr_parse(xmlLevels[xml_current_level].name, value, &scenario->initParams.cpr );
					}
					break;
				case PARSE_INIT_STATE_SCENE:
					if ( ( xml_current_level == 2 ) && 
						( strcmp(xmlLevels[xml_current_level].name, "initial_scene" ) == 0 ) )
					{
						simmgr_shm->status.scenario.scene_id = atoi(value );
						current_scene_id = simmgr_shm->status.scenario.scene_id;
						if ( verbose )
						{
							printf("Set Initial Scene to ID %d\n", current_scene_id );
						}
						sts = 0;
					}
					else if ( ( xml_current_level == 2 ) && 
						( strcmp(xmlLevels[xml_current_level].name, "scene" ) == 0 ) )
					{
						simmgr_shm->status.scenario.scene_id = atoi(value );
						current_scene_id = simmgr_shm->status.scenario.scene_id;
						if ( verbose )
						{
							printf("Set Initial Scene to ID %d\n", current_scene_id );
						}
						sts = 0;
					}
					break;
				default:
					sts = 0;
					break;
			}
			break;
			
		case PARSE_STATE_SCENE:
			switch ( parse_scene_state )
			{
				case PARSE_SCENE_STATE_NONE:
					if ( verbose )
					{
						printf("SCENE_STATE_NONE: Lvl %d Name %s, Value, %s\n",
							xml_current_level, xmlLevels[xml_current_level].name, value );
					}
					
					if ( xml_current_level == 2 )
					{
						if (strcmp(xmlLevels[2].name, "id" ) == 0 )
						{
							new_scene->id = atoi(value );
							if ( verbose )
							{
								printf("Set Scene ID to %d\n", new_scene->id );
							}
						}
						else if ( strcmp(xmlLevels[2].name, "title" ) == 0 )
						{
							sprintf(new_scene->name, "%s", value );
							if ( verbose )
							{
								printf("Set Scene Name to %s\n", new_scene->name );
							}
						}
					}
					sts = 0;
					break;
					
				case PARSE_SCENE_STATE_TIMEOUT:
					if ( xml_current_level == 3 )
					{
						if ( strcmp(xmlLevels[3].name, "timeout_value" ) == 0 )
						{
							new_scene->timeout = atoi(value);
						}
						else if ( strcmp(xmlLevels[3].name, "scene_id" ) == 0 )
						{
							new_scene->timeout_scene = atoi(value);
						}
					}
					break;
				
				
				case PARSE_SCENE_STATE_INIT_CARDIAC:
					if ( xml_current_level == 4 )
					{
						sts = cardiac_parse(xmlLevels[4].name, value, &new_scene->initParams.cardiac );
					}
					break;
				case PARSE_SCENE_STATE_INIT_RESPIRATION:
					if ( xml_current_level == 4 )
					{
						sts = respiration_parse(xmlLevels[4].name, value, &new_scene->initParams.respiration );
					}
					break;
				case PARSE_SCENE_STATE_INIT_GENERAL:
					if ( xml_current_level == 4 )
					{
						sts = general_parse(xmlLevels[4].name, value, &new_scene->initParams.general );
					}
					break;
				case PARSE_SCENE_STATE_INIT_VOCALS:
					if ( xml_current_level == 4 )
					{
						sts = vocals_parse(xmlLevels[4].name, value, &new_scene->initParams.vocals );
					}
					break;
				case PARSE_SCENE_STATE_INIT_MEDIA:
					if ( xml_current_level == 4 )
					{
						sts = media_parse(xmlLevels[4].name, value, &new_scene->initParams.media );
					}
					break;
				case PARSE_SCENE_STATE_INIT_CPR:
					if ( xml_current_level == 4 )
					{
						sts = cpr_parse(xmlLevels[4].name, value, &new_scene->initParams.cpr );
					}
					break;
				case PARSE_SCENE_STATE_TRIGS:
					break;
				case PARSE_SCENE_STATE_TRIG:
					if ( xml_current_level == 4 )
					{
						if ( strcmp(xmlLevels[4].name, "test" ) == 0 )
						{
							for ( i = 0 ; i <= TRIGGER_TEST_OUTSIDE ; i++ )
							{
								if ( strcmp(trigger_tests[i], value ) == 0 )
								{
									new_trigger->test = i;
									break;
								}
							}
						}
						else if ( strcmp(xmlLevels[4].name, "scene_id" ) == 0 )
						{
							new_trigger->scene = atoi(value );
						}
						else if ( strcmp(xmlLevels[4].name, "event_id" ) == 0 )
						{
							sprintf(new_trigger->param_element, "%s", value );
							new_trigger->test = TRIGGER_TEST_EVENT;
						}
						else
						{
							sts = 1;
						}
					}
					else if ( xml_current_level == 5 )
					{
						sprintf(new_trigger->param_class,"%s", xmlLevels[4].name );
						sprintf(new_trigger->param_element,"%s", xmlLevels[5].name );
						new_trigger->value = atoi(value );
						
						// For range, the two values are shown as "2-4". No spaces allowed.
						value2 = strchr(value, '-' );
						if ( value2 )
						{
							value2++;
							new_trigger->value2 = atoi(value2 );
						}
						if ( verbose )
						{
							printf("%s : %s : %d-%d\n", 
							new_trigger->param_class, new_trigger->param_element,
							new_trigger->value, new_trigger->value2 );
						}
					}
					else
					{
						sts = 2;
					}
					break;
				
					
				case PARSE_SCENE_STATE_INIT:
				default:
					if ( verbose )
					{
						printf("SCENE_STATE default (%d): Lvl %d Name %s, Value, %s\n",
							parse_scene_state, xml_current_level, xmlLevels[xml_current_level].name, value );
					}
					sts = -1;
					break;
			}
			break;
		
		case PARSE_STATE_EVENTS:
			if ( xml_current_level == 3 )
			{
				if ( strcmp(xmlLevels[3].name, "name" ) == 0 )
				{
					// Set the current Category Name
					sprintf(current_event_catagory, "%s", value );
				}
				else if ( strcmp(xmlLevels[3].name, "title" ) == 0 )
				{
					// Set the current Category Name
					sprintf(current_event_title, "%s", value );
				}
			}
			else if ( xml_current_level == 4 )
			{
				if ( strcmp(xmlLevels[4].name, "title" ) == 0 )
				{
					sprintf(new_event->event_title, "%s", value );
				}
				else if ( strcmp(xmlLevels[4].name, "id" ) == 0 )
				{
					sprintf(new_event->event_id, "%s", value );
				}
			}
			break;
		case PARSE_STATE_HEADER:
			if ( xml_current_level == 2 )
			{
				if ( strcmp(xmlLevels[2].name, "author" ) == 0 )
				{
					snprintf(scenario->author, LONG_STRING_SIZE, "%s", value );
				}
				else if ( strcmp(xmlLevels[2].name, "date_created" ) == 0 )
				{
					snprintf(scenario->date_created, NORMAL_STRING_SIZE, "%s", value );
				}
				else if ( strcmp(xmlLevels[2].name, "description" ) == 0 )
				{
					snprintf(scenario->description, LONG_STRING_SIZE, "%s", value );
				}
			}
			else if ( xml_current_level == 3 )
			{
				if ( strcmp(xmlLevels[3].name, "title" ) == 0 )
				{
					snprintf(scenario->title, LONG_STRING_SIZE, "%s", value );
				}
			}
		default:
			if ( strcmp(xmlLevels[1].name, "header" ) == 0 )
			{
				if ( xml_current_level == 2 )
				{
					name = xmlLevels[2].name;
					if ( strcmp(name, "author" ) == 0 )
					{
						sprintf(scenario->author, "%s", value );
						if ( verbose )
						{
							printf("Author: %s\n", scenario->author );
						}
					}
					else if ( strcmp(name, "date_of_creation" ) == 0 )
					{
						snprintf(scenario->date_created, NORMAL_STRING_SIZE, "%s", value );
						if ( verbose )
						{
							printf("Created: %s\n", scenario->date_created );
						}
					}
					else if ( strcmp(name, "description" ) == 0 )
					{
						snprintf(scenario->description, LONG_STRING_SIZE, "%s", value );
						if ( verbose )
						{
							printf("Description: %s\n", scenario->description );
						}
					}
					else
					{
						if ( verbose)
						{
							printf("In Header, Unhandled: Name is %s, Value, %s\n", xmlLevels[xml_current_level].name, value );
						}
					}
				}
				else if ( xml_current_level == 3 )
				{
					name = xmlLevels[3].name;
					if ( strcmp( name, "name" ) == 0 )
					{
						snprintf(scenario->title, LONG_STRING_SIZE, "%s", value );
						if ( verbose )
						{
							printf("Title: %s\n", scenario->title );
						}
					}
				}
				else
				{
					if ( verbose )
					{
						printf("In Header, Level is %d, Name is %s, Value, %s\n", xml_current_level, xmlLevels[xml_current_level].name, value );
					}
				}
			}
			break;
	}
	if ( sts && verbose )
	{
		printf("saveData STS %d: Lvl %d: %s, Value  %s, \n", sts, xml_current_level, xmlLevels[xml_current_level].name, value );
	}
}

/**
 * startParseState:
 * @lvl: New level from XML
 * @name: Name of the new level
 *
 * Process level change.
*/

static void
startParseState(int lvl, char *name )
{
	if ( verbose )
	{
		printf("startParseState: Cur State %s: Lvl %d Name %s   ", parse_states[parse_state], lvl, name );
		if ( parse_state == PARSE_STATE_INIT )
		{
			printf(" %s", parse_init_states[parse_init_state] );
		}
		else if ( parse_state == PARSE_STATE_SCENE )
		{
			printf(" %s", parse_scene_states[parse_scene_state] );
		}
	}
	switch ( lvl )
	{
		case 0:		// Top level - no actions
			break;
			
		case 1:	// profile, media, events have no action
			if ( strcmp(name, "init" ) == 0 )
			{
				parse_state = PARSE_STATE_INIT;
			}
			else if ( ( strcmp(name, "scene" ) == 0 ) || ( strcmp(name, "initial_scene" ) == 0 ) )
			{
				// Allocate a scene
				new_scene =  (struct scenario_scene *)calloc(1, sizeof(struct scenario_scene ) );
				initializeParameterStruct(&new_scene->initParams );
				insert_llist(&new_scene->scene_list, &scenario->scene_list );
				parse_state = PARSE_STATE_SCENE;
				if ( verbose )
				{
					printf("***** New Scene started ******\n" );
				}
			}
			else if ( strcmp(name, "events" ) == 0 )
			{
				parse_state = PARSE_STATE_EVENTS;
				sprintf(current_event_catagory, "%s", "" );
				sprintf(current_event_title, "%s", "" );
			}
			else if ( strcmp(name, "header" ) == 0 )
			{
				parse_state = PARSE_STATE_HEADER;
				sprintf(current_event_catagory, "%s", "" );
				sprintf(current_event_title, "%s", "" );
			}
			break;
			
		case 2:	// 
			switch ( parse_state )
			{
				case PARSE_STATE_INIT:
					if ( strcmp(name, "cardiac" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_CARDIAC;
					}
					else if ( strcmp(name, "respiration" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_RESPIRATION;
					}
					else if ( strcmp(name, "general" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_GENERAL;
					}
					else if ( strcmp(name, "vocals" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_VOCALS;
					}
					else if ( strcmp(name, "media" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_MEDIA;
					}
					else if ( strcmp(name, "cpr" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_CPR;
					}
					else if ( strcmp(name, "scene" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_SCENE;
					}
					else if ( strcmp(name, "initial_scene" ) == 0 )
					{
						parse_init_state = PARSE_INIT_STATE_SCENE;
					}
					else
					{
						parse_init_state = PARSE_INIT_STATE_NONE;
					}
					break;
					
				case PARSE_STATE_SCENE:
					if ( strcmp(name, "init" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT;
					}
					else if ( strcmp(name, "timeout" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_TIMEOUT;
					}
					else if ( strcmp(name, "triggers" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_TRIGS;
					}
					else
					{
						parse_scene_state = PARSE_SCENE_STATE_NONE;
					}
					break;
					
				case PARSE_STATE_HEADER:
					if ( strcmp(name, "author" ) == 0 )
					{
						parse_header_state = PARSE_HEADER_STATE_AUTHOR;
					}
					else if ( strcmp(name, "title" ) == 0 )
					{
						parse_header_state = PARSE_HEADER_STATE_TITLE;
					}
					else if ( strcmp(name, "date_of_creation" ) == 0 )
					{
						parse_header_state = PARSE_HEADER_STATE_DATE_OF_CREATION;
					}
					else if ( strcmp(name, "description" ) == 0 )
					{
						parse_header_state = PARSE_HEADER_STATE_DESCRIPTION;
					}
					break;
					
				default:
					break;
			}
			break;
			
		case 3:
			switch ( parse_state )
			{
				case PARSE_STATE_SCENE:
					if ( strcmp(name, "cardiac" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_CARDIAC;
					}
					else if ( strcmp(name, "respiration" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_RESPIRATION;
					}
					else if ( strcmp(name, "general" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_GENERAL;
					}
					else if ( strcmp(name, "vocals" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_VOCALS;
					}
					else if ( strcmp(name, "media" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_MEDIA;
					}
					else if ( strcmp(name, "cpr" ) == 0 )
					{
						parse_scene_state = PARSE_SCENE_STATE_INIT_CPR;
					}
					if ( ( parse_scene_state == PARSE_SCENE_STATE_TRIGS ) &&
						 ( strcmp(name, "trigger" ) == 0 ) )
					{
						new_trigger = (struct scenario_trigger *)calloc(1, sizeof(struct scenario_trigger ) );
						insert_llist(&new_trigger->trigger_list, &new_scene->trigger_list );

						parse_scene_state = PARSE_SCENE_STATE_TRIG;
						if ( verbose )
						{
							printf("***** New Trigger started ******\n" );
						}
					}
					break;
				case PARSE_STATE_EVENTS:
					if ( strcmp(name, "event" ) == 0 )
					{
						if ( verbose )
						{
							printf("New Event %s : %s\n",	current_event_catagory, current_event_title );
						}
						new_event = (struct scenario_event *)calloc(1, sizeof(struct scenario_event ) );
						insert_llist(&new_event->event_list, &scenario->event_list );
						
						sprintf(new_event->event_catagory_name, "%s", current_event_catagory );
						sprintf(new_event->event_catagory_title, "%s", current_event_title );
						
						if ( verbose )
						{
							printf("***** New Event started ******\n" );
						}
					}
					break;
				case PARSE_STATE_HEADER:
				case PARSE_STATE_INIT:
				case PARSE_STATE_NONE:
					break;
			}
			
			
			break;

		default:
			break;
	}
	if ( verbose )
	{
		printf("New State %s: ", parse_states[parse_state] );
		if ( parse_state == PARSE_STATE_INIT )
		{
			printf(" %s", parse_init_states[parse_init_state] );
		}
		else if ( parse_state == PARSE_STATE_SCENE )
		{
			printf(" %s", parse_scene_states[parse_scene_state] );
		}
		/*
		else if ( parse_state == PARSE_STATE_HEADER )
		{
			printf(" %s", parse_header_states[parse_header_state] );
		}
		*/
		printf("\n" );
	}
}
static void
endParseState(int lvl )
{
	if ( verbose )
	{
		printf("endParseState: Lvl %d    State: %s", lvl,parse_states[parse_state] );
		if ( parse_state == PARSE_STATE_INIT )
		{
			printf(" %s\n", parse_init_states[parse_init_state] );
		}
		else if ( parse_state == PARSE_STATE_SCENE )
		{
			printf(" %s\n", parse_scene_states[parse_scene_state] );
		}
		else
		{
			printf("\n" );
		}
	}
	switch ( lvl )
	{
		case 0:	// Parsing Complete
			break;
			
		case 1:	// Section End
			parse_state = PARSE_STATE_NONE;
			break;
			
		case 2:
			switch ( parse_state )
			{
				case PARSE_STATE_SCENE:
					parse_scene_state = PARSE_SCENE_STATE_NONE;
					break;
					
				case PARSE_STATE_INIT:
					parse_init_state = PARSE_INIT_STATE_NONE;
					break;
			}
			break;
		case 3:
			if ( ( parse_scene_state == PARSE_SCENE_STATE_TRIG ) &&
				 ( new_trigger ) )
			{
				new_trigger = (struct scenario_trigger *)0;
				parse_scene_state = PARSE_SCENE_STATE_TRIGS;
			}
			break;
			
		default:
			break;
	}
}
/**
 * processNode:
 * @reader: the xmlReader
 *
 * Dump information about the current node
 */
static void
processNode(xmlTextReaderPtr reader)
{
    const xmlChar *name;
	const xmlChar *value;
	int lvl;
	
    name = xmlTextReaderConstName(reader);
    if ( name == NULL )
	{
		name = BAD_CAST "--";
	}
	line_number = xmlTextReaderGetParserLineNumber(reader);
	
    value = xmlTextReaderConstValue(reader);
	// Node Type Definitions in http://www.gnu.org/software/dotgnu/pnetlib-doc/System/Xml/XmlNodeType.html
	
	switch ( xmlTextReaderNodeType(reader) )
	{
		case 1: // Element
			xml_current_level = xmlTextReaderDepth(reader);
			xmlLevels[xml_current_level].num = xml_current_level;
			if ( xmlStrlen(name) >= PARAMETER_NAME_LENGTH )
			{
				fprintf(stderr, "XML Parse Error: %s[%d]: Name %s exceeds Max Length of %d\n",
					xml_filename, line_number, name, PARAMETER_NAME_LENGTH-1 );
					
			}
			else
			{
				sprintf(xmlLevels[xml_current_level].name, "%s", name );
			}
			// printf("Start %d %s\n", xml_current_level, name );
			startParseState(xml_current_level, (char *)name );
			break;
		
		case 3:	// Text
			cleanString((char *)value );
			if ( verbose )
			{
				for ( lvl = 0 ; lvl <= xml_current_level ; lvl++ )
				{
					printf("[%d]%s:", lvl, xmlLevels[lvl].name );
				}
				printf(" %s\n", value );
			}
			saveData(name, value );
			break;
			
		case 13: // Whitespace
		case 14: // Significant Whitespace 
		case 8: // Comment
			break;
		case 15: // End Element
			// printf("End %d\n", xml_current_level );
			endParseState(xml_current_level );
			xml_current_level--;
			break;
		
			
		case 0: // None
		case 2: // Attribute
		case 4: // CDATA
		case 5: // EntityReference
		case 6: // Entity
		case 7: // ProcessingInstruction
		case 9: // Document
		case 10: // DocumentType
		case 11: // DocumentTragment
		case 12: // Notation
		case 16: // EndEntity
		case 17: // XmlDeclaration
		
		default:
			if ( verbose )
			{
				printf("Node: %d %d %s %d %d", 
					xmlTextReaderDepth(reader),
					xmlTextReaderNodeType(reader),
					name,
					xmlTextReaderIsEmptyElement(reader),
					xmlTextReaderHasValue(reader));
			
				if (value == NULL)
				{
					printf("\n");
				}
				else
				{
					if (xmlStrlen(value) > 60)
					{
						printf(" %.80s...\n", value);
					}
					else
					{
						printf(" %s\n", value);
					}
				}
			}
	}
}

/**
 * readScenario:
 * @filename: the file name to parse
 *
 * Parse the scenario
 */
 
static int
readScenario(const char *filename)
{
    xmlTextReaderPtr reader;
    int ret;

    xmlLineNumbersDefault(1);
    
	reader = xmlReaderForFile(filename, NULL, 0);
	if ( reader != NULL )
	{
		while ( ( ret = xmlTextReaderRead(reader) ) == 1 )
		{
            processNode(reader);
        }
        xmlFreeTextReader(reader);
        if (ret != 0)
		{
            fprintf(stderr, "%s : failed to parse\n", filename);
			return ( -1 );
        }
    } 
	else
	{
        fprintf(stderr, "Unable to open %s\n", filename);
		return ( -1 );
    }
	return ( 0 );
}
