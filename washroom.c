#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include "uthread.h"
#include "uthread_mutex_cond.h"

#define MAX_OCCUPANCY      3							// max occupancy of washroom
#define NUM_ITERATIONS     100							// number of times each person enters washroom
#define NUM_PEOPLE         20							// number of people using the washroom
#define FAIR_WAITING_COUNT 4							// when wait count hits 4, block entry so gender can swap

enum GenderIdentity {MALE = 0, FEMALE = 1};

struct Washroom {
	int male_wait_count;							// time male waits outside female washroom
	int female_wait_count; 							// time female waits outside male washroom
	int current;								// current occupancy of washroom
	int canEnter;								// indicates that there is space in washroom
	enum GenderIdentity gender;						// current gender that can enter
};

struct Washroom* createWashroom() {
  struct Washroom* washroom = malloc (sizeof (struct Washroom));
  washroom->male_wait_count = 0;
  washroom->female_wait_count = 0;
  washroom->current = 0;
  washroom->canEnter = 1;
  washroom->gender = MALE;
  return washroom;
}	

struct Washroom* washroom;

uthread_mutex_t w_lock;								// washroom mutex lock
uthread_cond_t spot_open;;							// indicates spot is open in washroom

#define WAITING_HISTOGRAM_SIZE (NUM_ITERATIONS * NUM_PEOPLE)
int             counter;                                          		// incremented with each entry, used to keep track of time waited
int             waitingHistogram         [WAITING_HISTOGRAM_SIZE];		// will contain info about how long each person waited to get in
int             waitingHistogramOverflow;
uthread_mutex_t waitingHistogramMutex;
int             occupancyHistogram       [2] [MAX_OCCUPANCY + 1];

void enterWashroom (enum GenderIdentity g) {
	counter++;			
	if(washroom->current == 0) {
		washroom->gender = g;						// set gender if washroom is empty
	}
	washroom->current++;							// increment current occupancy
	if(g){
		occupancyHistogram [FEMALE] [washroom->current] ++;		
	}
	else 
		occupancyHistogram [MALE] [washroom->current] ++;
	assert(washroom->gender == g);						// assertions to ensure that gender is same and current < MAX_OCCUPANCY
	assert(washroom->current <= MAX_OCCUPANCY);
}

void leaveWashroom() {
	uthread_mutex_lock(w_lock);						// acquire washroom lock
	washroom->current--;							// decrement current occupancy
	enum GenderIdentity gender = washroom->gender;
	if (gender == MALE){
		if (washroom->female_wait_count <= FAIR_WAITING_COUNT){
			uthread_cond_broadcast(spot_open);			// broadcast condition variable (in case first in queue is wrong gender)
		}
		if (washroom->female_wait_count > FAIR_WAITING_COUNT){
			washroom->canEnter = 0;					// don't broadcast if gender needs to swap b/c females waited too long, block entry
			if (washroom->current == 0){
				washroom->gender = FEMALE;			// when washroom empty: flip gender, reset wait count, allow entry
				washroom->female_wait_count = 0;
				washroom->canEnter = 1;
				uthread_cond_broadcast(spot_open);		// broadcast condition variable
			}
		}
	}
	else if (gender == FEMALE){
		if (washroom->male_wait_count <= FAIR_WAITING_COUNT){
			uthread_cond_broadcast(spot_open);			// broadcast condition variable (in case first in queue is wrong gender)
		}
		if (washroom->male_wait_count > FAIR_WAITING_COUNT){
			washroom->canEnter = 0;					// don't broadcast if gender needs to swap b/c females waited too long, block entry
			if (washroom->current == 0){
				washroom->gender = MALE;			// when washroom empty: flip gender, reset wait count, allow entry
				washroom->male_wait_count = 0;
				washroom->canEnter = 1;
				uthread_cond_broadcast(spot_open);		// broadcast condition variable
			}
		}
	}
	uthread_mutex_unlock(w_lock);						// release washroom mutex
}

int tryEnter(enum GenderIdentity g){
	int start_time = 0; 
	int waitingTime = 0; 
	int end_time = 0;
	uthread_mutex_lock(w_lock);						// acquire washroom mutex
	while(1){
		if (washroom->canEnter && washroom->current != 0 && washroom->current < MAX_OCCUPANCY && washroom->gender != g){
			if(g == 1){
				washroom->female_wait_count++;			// increment if female waiting cannot enter because washroom is male
			}
			else if(g == 0){
				washroom->male_wait_count++;			// increment if male waiting cannot enter because washroom is female
			}
		}
		else if((washroom->canEnter && washroom->current == 0) || (washroom->canEnter && washroom->current < MAX_OCCUPANCY && washroom->gender == g))
			break;							// break if washroom is empty or space available with correct gender
		if (start_time == 0)
			start_time = counter; 					// set start time if person is washroom is unavailable
		uthread_cond_wait(spot_open);					// set thread to wait for condition variable
	}
	end_time = counter;							// set end time for when person actually enters
	if(start_time)
		waitingTime = end_time - start_time;				// calculate time person waited for washroom
	uthread_mutex_lock(waitingHistogramMutex);				// lock histogram mutex	
	if (waitingTime < WAITING_HISTOGRAM_SIZE){
		waitingHistogram [waitingTime] ++;				// increment appropriate spot in histogram
	}
	else
		waitingHistogramOverflow ++;
	uthread_mutex_unlock(waitingHistogramMutex);				// release histogram mutex					
	enterWashroom(g);							// enter washroom
	uthread_mutex_unlock(w_lock);						// release washroom mutex
}
void * person (void* av) {
	intptr_t z = (intptr_t) av;
	intptr_t i = z % 2;							// even numbers are male, odds are female
	for (int k = 0; k < NUM_ITERATIONS; k++){				// will enter the washroom NUM_ITERATIONS times
		tryEnter(i);
		for(int n=0; n < NUM_PEOPLE; n++){				// after successfully entering the washroom, yields -
			uthread_yield();					// to others NUM_ITERATAIONS times before leaving
		}
		leaveWashroom();						// leave washroom
		for(int n=0; n < NUM_PEOPLE; n++){
			uthread_yield();					// after leaving the washroom, yields to others -
		}								// NUM_ITERATIONS times before attempting to reenter
	}
}

int main (int argc, char** argv) {
	uthread_init (NUM_PEOPLE);						// initialize threads that will each represent a single person
	washroom = createWashroom();
	uthread_t pt [NUM_PEOPLE];						// array that will store our threads for later access (joining)

  	waitingHistogramMutex = uthread_mutex_create ();
  	w_lock = uthread_mutex_create();					// create mutexes and condition variable
  	spot_open = uthread_cond_create(w_lock);

  	for (int i=0; i<NUM_PEOPLE; i++){
		int r = random() % 100;						// generate random number, create male if r is even, else female
		uthread_t u = uthread_create(person, (void*) (intptr_t) r);	// create thread that will be the person trying to enter washroom
		pt[i] = u;							// add thread to array
  	}
	
  	for (int i=0; i<NUM_PEOPLE; i++){
		uthread_t u = pt[i];
		uthread_join(u, 0);						// try to join threads
  	}


  	uthread_mutex_lock(waitingHistogramMutex);				// output histogram information
  	printf ("Times with 1 person who identifies as male   %d\n", occupancyHistogram [MALE]   [1]);
  	printf ("Times with 2 people who identifies as male   %d\n", occupancyHistogram [MALE]   [2]);
  	printf ("Times with 3 people who identifies as male   %d\n", occupancyHistogram [MALE]   [3]);
  	printf ("Times with 1 person who identifies as female %d\n", occupancyHistogram [FEMALE] [1]);
  	printf ("Times with 2 people who identifies as female %d\n", occupancyHistogram [FEMALE] [2]);
  	printf ("Times with 3 people who identifies as female %d\n", occupancyHistogram [FEMALE] [3]);
 	printf ("Waiting Histogram\n");
 	for (int i=0; i<WAITING_HISTOGRAM_SIZE; i++)
    		if (waitingHistogram [i])
      			printf ("  Number of times people waited for %d %s to enter: %d\n", i, i==1?"person":"people", waitingHistogram [i]);
  		if (waitingHistogramOverflow)
    			printf ("  Number of times people waited more than %d entries: %d\n", WAITING_HISTOGRAM_SIZE, waitingHistogramOverflow);
	uthread_mutex_unlock(waitingHistogramMutex);
	free(washroom);
	uthread_mutex_destroy(waitingHistogramMutex);
	uthread_mutex_destroy(w_lock);
	uthread_cond_destroy(spot_open);
}

