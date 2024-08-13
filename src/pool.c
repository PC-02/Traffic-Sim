#include <stdlib.h>
#include <stdio.h>
#include "mpi.h"
#include "../include/pool.h"
#include "../include/code.h"


// Example command package data type which can be extended
static MPI_Datatype MESSAGE;
static struct Message recv_message;
static struct Message sync_message;
static MPI_Request command_req = MPI_REQUEST_NULL;

// used by the master to track process statuses
static int num_of_proc;
static int *proc_tracker;
static int proc_request_count;

// rank of synchronizer process
static int proc_to_run_sim = 1;

MPI_Comm new_communicator;

static void errorMessage(char*);
static void createCommandType();
static int handleRecievedCommand();
static int startAwaitingProcessesIfNeeded(int, int);
static struct Message createMessage(enum Message_Type);
static int checkAllSleep();


int rank, sim_size, sim_workers;
int proc_offset = 2;


// pool specific tasks, for debugging/logging
char* pool_task_descriptions[] = {
  "Start Simulation",
  "Shutdown Process",
  "Shutdown Pool",
};


int processPoolInit(){
  createCommandType();
  MPI_Group world_group;
  MPI_Comm_group(MPI_COMM_WORLD, &world_group);
  
  int *ranks= malloc((num_of_proc - 1) * sizeof(int));
  
  for(int i=1; i<num_of_proc; i++){
    ranks[i - 1] = i;
  }

  // create a group of processes that will be used to run the sim (all procs excluding master)
  MPI_Group new_group;
  MPI_Group_incl(world_group, num_of_proc - 1, ranks, &new_group);
 
  if(DEBUG_MODE){
    int grp_size;
    MPI_Group_size(new_group, &grp_size);
    printf("[Process %d] Initialized. %d procs are being used to run the sim\n", rank, grp_size);
  }

  if(rank == 0){

    printf("[Master] Running program with %d processes\n", num_of_proc);

    if(num_of_proc < 3){
      errorMessage("No worker processes available for pool, run with more than two MPI process");
    }
    
    // empty group for master
    MPI_Comm_create(MPI_COMM_WORLD, MPI_GROUP_EMPTY, &new_communicator);

    proc_tracker = malloc(num_of_proc * sizeof(int));

    for(int i = 0; i < num_of_proc - 1; i++){
      
      if(i == proc_to_run_sim - 1){
        if(DEBUG_MODE){
          printf("[Master] Process %d reserved to run simulation\n", proc_to_run_sim);
        }
        proc_tracker[i] = 1;
      }
      else{
        proc_tracker[i] = 0;
      }
      
    } 

    proc_request_count = 0;

    if(DEBUG_MODE){
      printf("[Master] Initialised Master\n");
    }

    return 2;
  } 
  else{
    // new communicator for sim workers
    MPI_Comm_create(MPI_COMM_WORLD, new_group, &new_communicator);    
    MPI_Recv(&recv_message, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    return handleRecievedCommand();
  }
}


// informs the synchronizer to start the main code, in this case it is a simulation
void startSim(){
  if(rank == 0){
    struct Message message = createMessage(PP_WAKE);
    message.parent = 0;
    message.task_id = START_SIM;

    if(DEBUG_MODE){
      printf("[Master] Starting process %d simulation\n", proc_to_run_sim);
    }

    MPI_Send(&message, 1, MESSAGE, proc_to_run_sim, MESSAGE_TAG, MPI_COMM_WORLD);
  }
  else{
    errorMessage("Only master process can start the simulation.");
    return;
  }
    
}


// derived datatype creation
void createCommandType(){
	struct Message message;

  MPI_Aint msgAddr, parentAddr, taskAddr;
	MPI_Get_address(&message, &msgAddr);
	MPI_Get_address(&message.parent, &parentAddr);
	MPI_Get_address(&message.task_id, &taskAddr);

  int items = 3;
	int blocklengths[3] = {1,1,1};

	MPI_Datatype types[3] = {MPI_CHAR, MPI_INT, MPI_INT};

  // find the exact offset required to get to the data address inside the message structure.
	MPI_Aint offsets[3] = {0, parentAddr - msgAddr, taskAddr - msgAddr};

	MPI_Type_create_struct(items, blocklengths, offsets, types, &MESSAGE);
	MPI_Type_commit(&MESSAGE);
  return;
}


// constantly listens for messages incoming and handles them accordingly
int masterCode(){
  if(rank == 0){
    MPI_Status status;
		MPI_Recv(&recv_message, 1, MESSAGE, MPI_ANY_SOURCE, MESSAGE_TAG, MPI_COMM_WORLD, &status);

		if(recv_message.type == PP_SLEEPING){
      
      if(DEBUG_MODE){
        printf(
          "[Master] Received sleep command from %d after finishing task: %d\n",
          status.MPI_SOURCE, task_descriptions[recv_message.task_id]
        );
      } 
      proc_tracker[status.MPI_SOURCE-1] = 0;

      return 1;
		}

		if(recv_message.type == PP_RUNCOMPLETE){
			
      if(DEBUG_MODE){
        printf("[Master] Received shutdown command\n"); 
      } 
			return 0;
		}

    if(recv_message.type == PP_CHECKSTOP){
      int sleep = checkAllSleep();
      MPI_Send(&sleep, 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD);
      return 1;
    }

		if(recv_message.type == PP_STARTPROCESS){
			proc_request_count++;
		
      int return_rank = -1;

      while(return_rank == -1){
        return_rank = startAwaitingProcessesIfNeeded(recv_message.parent, recv_message.task_id);
      } 

			// If the master was to start a worker then send back the process rank that this worker is now on
			MPI_Send(&return_rank, 1, MPI_INT, status.MPI_SOURCE, PP_PID_TAG, MPI_COMM_WORLD);
		}

		return 1;
  }
  else{
    errorMessage("Worker process called master, this shouldn't happen");
    return 0;
  }
}


// check with master if all workers are done running current task
void checkComplete(){
  struct Message msg = createMessage(PP_CHECKSTOP);
  int status = 0;

  while(!status){
    MPI_Send(&msg, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD);
    MPI_Recv(&status, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  }
}


// checks if all workers are in sleep status currently
int checkAllSleep(){
  for(int i=0; i<num_of_proc - 1; i++){
    if((i + 1) != proc_to_run_sim){
      if(proc_tracker[i] != 0){
        return 0;
      }
    }
  }
  return 1;
}


// shut down all processes
void processPoolFinalise() {
	if (rank == 0) {
		if (proc_tracker != NULL){
      free(proc_tracker);
    }

		for(int i = 1; i < num_of_proc; i++) {
			
      if(i != proc_to_run_sim){
        if(DEBUG_MODE){
          printf("[Master] Shutting down process %d\n", i);
        }
			  struct Message msg = createMessage(PP_STOP);
        msg.task_id = SHUT_PROC;
			  MPI_Send(&msg, 1, MESSAGE, i, MESSAGE_TAG, MPI_COMM_WORLD);
      }
		}
	}
	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Type_free(&MESSAGE);
}


/**
 * A worker or the master can instruct to start another worker process
 */
int startWorkerProcess(int task) {
	if(rank == 0){
		proc_request_count++;
    int return_rank = -1;

    do{
		  return_rank = startAwaitingProcessesIfNeeded(rank, task);
    } while(return_rank == -1);

		return return_rank;
	} 
  else{
		int worker_rank;
		struct Message create_message = createMessage(PP_STARTPROCESS);
    create_message.task_id = task;
    create_message.parent = rank;
		MPI_Send(&create_message, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD);
		MPI_Recv(&worker_rank, 1, MPI_INT, 0, PP_PID_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  
		return worker_rank;
	}
}


int handleRecievedCommand(){
	// We have received a command and decide what to do

	if(recv_message.type == PP_WAKE){
		// If we are told to wake then post a recv for the next command and return true to continue
		MPI_Irecv(&recv_message, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD, &command_req);

		if(DEBUG_MODE){
      if(recv_message.task_id < 0){
        printf("[Worker] Process %d woken to work on pool task: %s\n", rank, pool_task_descriptions[recv_message.task_id]);
      }
      else{
        printf("[Worker] Process %d woken to work on task: %s\n", rank, task_descriptions[recv_message.task_id]);
      }
    }

    if(rank == proc_to_run_sim){
      return 3;
    }
    else{
      return 1;
    }

	} 
  else if(recv_message.type == PP_STOP){
		// Stopping so return zero to denote stop
		if(DEBUG_MODE){
      printf("[Worker] Process %d commanded to stop\n", rank);
    }
		return 0;
	} 
  else{
		errorMessage("Unexpected control command");
		return 0;
	}
}


// called by worker after task completion to set status to sleep
int workerSleep() {
	if (rank != 0) {
		if (recv_message.type==PP_WAKE) {

			// The command was to wake up, it has done the work and now it needs to switch to sleeping mode
			struct Message msg = createMessage(PP_SLEEPING);
      
      if(DEBUG_MODE){
        printf("[Worker] Process %d waiting for sync after task: %s\n", rank, task_descriptions[recv_message.task_id]);
      }

      MPI_Recv(&sync_message, 1, MESSAGE, proc_to_run_sim, recv_message.task_id, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      msg.task_id = recv_message.task_id;

      MPI_Send(&msg, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD);
      // MPI_Barrier(new_communicator);     
      if(DEBUG_MODE){
        printf("[Worker] Process %d done waiting for sync after task: %s\n", rank, task_descriptions[recv_message.task_id]);
      }

      if(command_req != MPI_REQUEST_NULL){
        MPI_Wait(&command_req, MPI_STATUS_IGNORE);
      }
		}
		return handleRecievedCommand();
	} else {
		errorMessage("Master process called worker poll");
		return 0;
	}
}

int getProcessParent() {
	return recv_message.parent;
}


// synchronize with a specific process after a specific task has completed
void sendSyncMessage(int proc_id, int task_id){   
  
  if(DEBUG_MODE){
    printf("[Worker] Attempting to sync proc %d after task: %s\n", proc_id, task_descriptions[task_id]);
  }

  MPI_Send(&sync_message, 1, MESSAGE, proc_id, task_id, MPI_COMM_WORLD);
}

/**
 * Called by the master and will start awaiting processes (signal to them to start) if required.
 * By default this works as a process pool, It takes the parent that requires a process and the task to be done
 * by that process
 * */
int startAwaitingProcessesIfNeeded(int parent, int task) {
	int waiting_proc_rank = -1;

  // replace with hash check of processes
	for(int i = 0; i < num_of_proc - 1; i++){
    if(!proc_tracker[i]){

      proc_tracker[i] = 1;
			struct Message message = createMessage(PP_WAKE);

			message.parent = parent;
      message.task_id = task;

      if(DEBUG_MODE){
        printf("[Master] Starting process %d for rank %d\n", i+1, parent);
      }

	    MPI_Send(&message, 1, MESSAGE, i+1, MESSAGE_TAG, MPI_COMM_WORLD);
			
      waiting_proc_rank = i+1;	// Will return this rank to the caller
      
			proc_request_count--;

			if(proc_request_count == 0){
        break;
      }
		}
		if(i == (num_of_proc - 2)){
        return -1;
        // return -1 and try again if no process found
			}
		}

	return waiting_proc_rank;
}


// request the master worker to shutdown all procs
void shutdownPool() {
	if(rank != 0) {
		if(DEBUG_MODE){
      printf("[Worker] Commanding a pool shutdown\n");
    }

		struct Message msg = createMessage(PP_RUNCOMPLETE);
    msg.task_id = SHUT_POOL;
		MPI_Send(&msg, 1, MESSAGE, 0, MESSAGE_TAG, MPI_COMM_WORLD);
	}
}

/**
 * A helper function which will create a command package from the desired command
 */
struct Message createMessage(enum Message_Type type) {
	struct Message msg;
	msg.type = type;
	return msg;
}


// throws an error
static void errorMessage(char * message) {
  fprintf(stderr,"[Proc %d] %s\n", rank, message);
  MPI_Abort(MPI_COMM_WORLD, 1);
}


// execute a given task
void sendMessage(enum Task type, int count){
  
  if(DEBUG_MODE){
    printf("[Worker] Proc %d attempting to start worker for task: %s\n", rank, task_descriptions[type]);
  }

  int data[2] = {type, count};
  int workerPid = startWorkerProcess(type);

  if(DEBUG_MODE){
    printf("[Worker] Attempting to set proc %d to work on task: %s \n", workerPid, task_descriptions[type]);
  }

	MPI_Send(data, 2, MPI_INT, workerPid, 100, MPI_COMM_WORLD);	

  if(DEBUG_MODE){
    printf("[Worker] Proc %d set to work on task: %s \n", workerPid, task_descriptions[type]);
  }
}


// this function is called by processes that are ONLY responsible for code synchronization
// these processes may perform some internal data syncs to ensure that the algorithms can 
// utilize global data but other than those they do not care for synchronization in the 
// sense of the global simulation execution.
void workerCode() {	
	int workerStatus = 1, data[2];

	while (workerStatus) {
    // figure out who is waking up this process, this lets us know who to listen to for
    // a task command.
		int parentId = getProcessParent();
 
		MPI_Recv(data, 2, MPI_INT, parentId, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if(DEBUG_MODE){
      printf(
        "[Worker] Proc %d received command from %d to work on task: %s\n", 
        rank, parentId, task_descriptions[data[0]]
      );
    }

    // call task mapping to complete some work for this process
    taskMapping(data[0], data[1]);

    if(DEBUG_MODE){
      printf("[Worker] Proc %d going to sleep after finish task: %s\n", rank, task_descriptions[data[0]]);
    }

    // try to go to sleep after finishing the task.
		workerStatus=workerSleep();
    
	}
}


// this function is called by the synchronizer process. This process is responsible for going
// through the simulation loop and sending batches of work to the master process. This process
// also takes part in the simulation to ensure load balancing and also ensures global synchronization
// accross the simulation. This is needed to ensure algorithms that require global data to function
// are in sync for all processes and that work is evenly divided amongst processes.
void synchronizerCode(enum Task type, int count) {	

  if(DEBUG_MODE){
    printf("[Worker] Proc %d set to work on task: %s \n", rank, task_descriptions[type]);
  }

  // call task mapping to complete some work for this process
  taskMapping(type, count);

  if(DEBUG_MODE){
    printf(
      "[Worker] Proc %d finished working on task: %s. Performing synchronization checks now\n",
      rank, task_descriptions[type]
    );
  }

  // send synchronization messages to all processes, this allows processes that are done with 
  // their work to indicate to the master that they can go to sleep.
  for(int i=0; i<sim_size; i++){
    sendSyncMessage(i + proc_offset, type);
  }
  // check to make sure all processes have indicated that they are asleep before starting next 
  // task. This is needed so that some processes do not take work from others.
  checkComplete();

}


void mpiBaseSetup(int* argc, char * argv[], int *proc_type){
  MPI_Init(argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_of_proc);

  *proc_type = processPoolInit();

  sim_size = num_of_proc - proc_offset;  // all sim workers that ONLY focus on sim tasks (master and synchronizer are excluded)
  sim_workers = num_of_proc - 1;         //subtract master work that does not do any sim work

}

// teardown function to free memory and datatypes and finalize mpi
void mpiTeardown(){
  MPI_Finalize();
}
