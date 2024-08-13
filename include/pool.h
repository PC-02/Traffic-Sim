#ifndef POOL_H_
#define POOL_H_


// MPI P2P tag to use for command communications, it is important not to reuse this
#define MESSAGE_TAG 16384
#define PP_PID_TAG 16383
#define DEBUG_MODE 0


#include "code.h"
#include "mpi.h"

// The core process pool command which instructs what to do next
enum Message_Type{
	PP_STOP=0,
	PP_SLEEPING=1,
	PP_WAKE=2,
	PP_STARTPROCESS=3,
	PP_RUNCOMPLETE=4,
	PP_CHECKSTOP=5
};

// tasks specific to the pool
enum Pool_Task{
	START_SIM=-1,
	SHUT_PROC=-2,
	SHUT_POOL=-3,
};

// process types
enum Proc_Type{
	WORKER=1,
	MASTER=2,
	SYNCHRONIZER=3,
};

extern MPI_Comm new_communicator; 
extern int rank, sim_size, sim_workers, proc_offset;

// An example data package which combines the command with some optional data, an example and can be extended
struct Message{
	enum Message_Type type;
	int parent;
    int task_id;
};

// Initialises the process pool
int processPoolInit();
// Finalises the process pool
void processPoolFinalise();
// Called by the master in loop, blocks until state change, 1=continue and 0=stop
int masterCode();
// Called by worker once task completed and will sleep until it receives instructions, 1=continue with new task and 0=stop
int workerSleep();
// Called by the master or a worker to start a new worker process
int startWorkerProcess();
// Called by a worker to shut the pool down
void shutdownPool();
// Retrieves the optional data associated with the command, provides an example of how this can be done
int getCommandData();
// sets up MPI
void mpiBaseSetup(int *, char *[], int *);
// start synchronizer
void startSim();
// start execution of provided task
void sendMessage(enum Task, int);
// run task for the synchronizer process
void synchronizerCode(enum Task, int);
// sync with a process
void sendSyncMessage(int, int);
// check if all processes are sleeping
void checkComplete();
// run by all workers 
void workerCode();
// finalize MPI
void mpiTeardown();

#endif /* POOL_H_ */
