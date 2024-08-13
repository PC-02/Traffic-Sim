#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>
#include "../include/pool.h"
#include "../include/code.h"
#include "mpi.h"

enum ReadMode {
  NONE,
  ROADMAP,
  TRAFFICLIGHTS
};

enum VehicleType {
  CAR,
  BUS,
  MINI_BUS,
  COACH,
  MOTORBIKE,
  BIKE
};

struct JunctionStruct {
  int id, num_roads, num_vehicles;
  char hasTrafficLights;
  int trafficLightsRoadEnabled;
  int total_number_crashes, total_number_vehicles;
  struct RoadStruct * roads;
};

struct RoadStruct {
  struct JunctionStruct *from, *to;
  int roadLength, maxSpeed, numVehiclesOnRoad, currentSpeed;
  int total_number_vehicles, max_concurrent_vehicles, id;
};

struct VehicleStruct {
  int passengers, source, dest, maxSpeed;
  // Distance is in meters
  int speed, arrived_road_time, fuel;
  time_t last_distance_check_secs, start_t;
  double remaining_distance;
  char active;
  struct JunctionStruct * currentJunction;
  struct RoadStruct * roadOn;
};


// the following data structures are used for synchronization
struct Data {
    // vehicle data
    int total_vehicles, vehicles_crashed, vehicles_exhausted_fuel;
    // passenger data
    int passengers_delivered, passengers_stranded;
};


struct VehicleData{
  int num_vehicles_on_road;
  int num_vehicles_on_junction;
  int junction_id;
  int road_id;
};

struct JunctionData{
  int num_vehicles_on_junction;
  int junction_id; 
  int crashed_vehicles_on_junction;
};

struct RoadData{
  int num_vehicles_on_road;
  int road_id;
  int max_concurrent_vehicles_on_road;
};


struct JunctionStruct * roadMap;
struct VehicleStruct * vehicles;
struct VehicleData vehicle_data[MAX_VEHICLES];
struct VehicleData *all_vehicle_data;
struct RoadData *all_road_data;
struct JunctionData *all_junction_data;

struct Data globalData;

int num_junctions=0, num_roads=0;
int elapsed_mins=0;

// access these values from the framework
extern int rank, sim_size, sim_workers, proc_offset;
extern MPI_Comm new_communicator;


// define synchronization operations 
MPI_Op Sync_Data;
MPI_Datatype DATA;

MPI_Op Sync_Vehicle_Data;
MPI_Datatype VEHICLE_DATA;

MPI_Op Sync_Junction_Data;
MPI_Datatype JUNCTION_DATA;

MPI_Op Sync_Road_Data;
MPI_Datatype ROAD_DATA;

// task descriptions for debugging / logging
char* task_descriptions[] = {
  "Initialize Vehicles",
  "Generate Vehicles",
  "Update Roads",
  "Update Vehicles",
  "Synchronize Data"
};

static void handleVehicleUpdate(int);
static int findAppropriateRoad(int, struct JunctionStruct*);
static int planRoute(int, int);
static int findIndexOfMinimum(double*, char*);
static void initVehicles(int);
static int activateRandomVehicle();
static int activateVehicle(enum VehicleType);
static int findFreeVehicle();
static void loadRoadMap(char*);
static int getRandomInteger(int, int);
static void writeDetailedInfo();
static time_t getCurrentSeconds();
static void updateRoad();
static void updateVehicle();
static void mpiSetup();
static void mpiFree();


// to be defined by the user based on the requirements for synchronization, this is used to sync the data 
// that is periodically printed to the terminal
void globalSyncData(void* elements_a, void* elements_b, int* length, MPI_Datatype* datatype){

  struct Data* input = (struct Data*) elements_a;
  struct Data* output = (struct Data*) elements_b;
  
  if(*datatype != DATA){
    printf("Please use global data for syncing.\n");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }

  output->total_vehicles = output->total_vehicles + input->total_vehicles;
  output->vehicles_crashed = output->vehicles_crashed + input->vehicles_crashed;
  output->vehicles_exhausted_fuel = output->vehicles_exhausted_fuel + input->vehicles_exhausted_fuel;
  output->passengers_delivered = output->passengers_delivered + input->passengers_delivered;
  output->passengers_stranded = output->passengers_stranded + input->passengers_stranded;

}


// same as above, user defined reduction function to make synchronization more efficient, this is used to
// sync vehicle data before road and junction updates
void globalSyncVehicleData(void* elements_a, void* elements_b, int* length, MPI_Datatype* datatype){

  struct VehicleData* input = (struct VehicleData*) elements_a;
  struct VehicleData* output = (struct VehicleData*) elements_b;
  
  if(*datatype != VEHICLE_DATA){
    printf("Please use vehicle data for syncing.\n");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }
  
  for(int i=0; i<*length; i++){
    for(int j=0; j<*length; j++){
      if(output[i].junction_id == input[j].junction_id){
        output[i].num_vehicles_on_junction += input[j].num_vehicles_on_junction;
        break;
      }
      if(output[i].road_id == input[j].road_id ){
        output[i].num_vehicles_on_road += input[j].num_vehicles_on_road;
        break;
      }
    }
  }
}



// same as above, user defined reduction function to make synchronization more efficient, this is used
// to sync road data before the final result is written into a file
void globalSyncRoadData(void* elements_a, void* elements_b, int* length, MPI_Datatype* datatype){

  struct RoadData* input = (struct RoadData*) elements_a;
  struct RoadData* output = (struct RoadData*) elements_b;
  
  if(*datatype != ROAD_DATA){
    printf("Please use road data for syncing.\n");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }
  
  for(int i=0; i<*length; i++){
    for(int j=0; j<*length; j++){
      if(output[i].road_id == input[j].road_id ){
        output[i].num_vehicles_on_road += input[j].num_vehicles_on_road;
        output[i].max_concurrent_vehicles_on_road += input[j].max_concurrent_vehicles_on_road;
        break;
      }
    }
  }
}



// same as above, user defined reduction function to make synchronization more efficient, this is used 
// to sync juntion data before the final result is written to a file
void globalSyncJunctionData(void* elements_a, void* elements_b, int* length, MPI_Datatype* datatype){
  // printf("%d\n", *length);
  struct JunctionData* input = (struct JunctionData*) elements_a;
  struct JunctionData* output = (struct JunctionData*) elements_b;
  
  if(*datatype != JUNCTION_DATA){
    printf("Please use junction data for syncing.\n");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }
  
  for(int i=0; i<*length; i++){
    for(int j=0; j<*length; j++){
      if(output[i].junction_id == input[j].junction_id){
        output[i].num_vehicles_on_junction += input[j].num_vehicles_on_junction;
        output[i].crashed_vehicles_on_junction += input[j].crashed_vehicles_on_junction;
        break;
      }
    }
  }
}

 
/**
 * Program entry point and main loop
 **/
int main(int argc, char * argv[]){

  if(argc != 2){
    if(rank == 0){
      fprintf(stderr, "Error: You need to provide the roadmap file as the only argument\n");
    }
    exit(-1);
  }

  // random num gen
  srand(time(0));

  int proc_type;
  double start_time;

  // set up command for the framework
  mpiBaseSetup(&argc, argv, &proc_type);
  // set up derived data types for the simulation
  mpiSetup();

  // Load road map graph (and traffic lights) from file
  loadRoadMap(argv[1]);
  if(rank == 0){
    printf("Loaded road map from file\n");
  }

  if(proc_type == MASTER){ 
    start_time = MPI_Wtime();

    startSim();
    int masterStatus = masterCode();

		while (masterStatus) {
			masterStatus=masterCode();
		}
   
  }
  else if(proc_type == SYNCHRONIZER){

    // initialize vehicles for all sim workers
    for(int i=0; i<sim_size; i++){
      sendMessage(initialize_vehicles, INITIAL_VEHICLES / sim_workers);
    }
    synchronizerCode(initialize_vehicles, INITIAL_VEHICLES / sim_workers);
 
    // Initialise time
    time_t seconds=0;
    time_t start_seconds=getCurrentSeconds();
    
    printf("Starting simulation with %d junctions, %d roads and %d vehicles\n", num_junctions, num_roads, INITIAL_VEHICLES);

    while (elapsed_mins < MAX_MINS){

      time_t current_seconds=getCurrentSeconds();
      
      if(current_seconds != seconds){
        seconds=current_seconds;

        if(seconds-start_seconds > 0){
          if((seconds-start_seconds) % MIN_LENGTH_SECONDS == 0){

            elapsed_mins++;

            // Add a random number of new vehicles to the simulation
            int num_new_vehicles=getRandomInteger(100, 200);

            // generate vehicles for all sim workers
            for(int i=0; i<sim_size; i++){
              sendMessage(generate_vehicles, num_new_vehicles / sim_workers);
            }
            synchronizerCode(generate_vehicles, num_new_vehicles / sim_workers);
        
            if(elapsed_mins % SUMMARY_FREQUENCY == 0){
              struct Data data_sum;

              // make all sim workers sync their data to rank 1 before the print
              for(int i=0; i<sim_size; i++){
                sendMessage(synchronize_data, 0);
              }
              
              MPI_Reduce(&globalData, &data_sum, 1, DATA, Sync_Data, 0, new_communicator);
              // 0 here because the rank is w.r.t the new comm
              
              // manually sync the worker processes after task completion
              for(int i=0; i<sim_size; i++){
                sendSyncMessage(i + proc_offset, synchronize_data);
              }
              checkComplete();

              printf("[Time: %d mins] %d vehicles, %d passengers delivered, %d stranded passengers, %d crashed vehicles, %d vehicles exhausted fuel\n",
                elapsed_mins, data_sum.total_vehicles, data_sum.passengers_delivered, data_sum.passengers_stranded, data_sum.vehicles_crashed, data_sum.vehicles_exhausted_fuel);
            }
          }
        }
      }

      // State update for junctions
      for(int i=0; i<sim_size; i++){
        sendMessage(update_roads, 0);
      }
      synchronizerCode(update_roads, 0);

      // Perform updates on vehicles
      for(int i=0; i<sim_size; i++){
        sendMessage(update_vehicles, 0);
      }
      synchronizerCode(update_vehicles, 0);

    }

    // inform master rank to stop checking for messages and begin shutdown
    shutdownPool();
  }
  else{
    workerCode();
  }

  // shut down all processes so everyone runs the below code
  processPoolFinalise();
 
  struct Data final_data;

  // reduce into final data struct for basic info
  MPI_Reduce(&globalData, &final_data, 1, DATA, Sync_Data, 0, MPI_COMM_WORLD);
  
  all_road_data = malloc(sizeof(struct RoadData) * num_roads);
  all_junction_data = malloc(sizeof(struct JunctionData) * num_junctions);
  
  int total_road_offset = 0;

  // locally compute the total result data required
  for(int i=0;i<num_junctions;i++){
    
    all_junction_data[i].junction_id = roadMap[i].id;
    all_junction_data[i].num_vehicles_on_junction = roadMap[i].total_number_vehicles;
    all_junction_data[i].crashed_vehicles_on_junction = roadMap[i].total_number_crashes;
    
    for(int j=0; j<roadMap[i].num_roads; j++){
 
      // set road data
      all_road_data[total_road_offset].road_id = roadMap[i].roads[j].id;
      all_road_data[total_road_offset].num_vehicles_on_road = roadMap[i].roads[j].total_number_vehicles;
      all_road_data[total_road_offset].max_concurrent_vehicles_on_road =  roadMap[i].roads[j].max_concurrent_vehicles;

      total_road_offset++;
    }
    
  }

  // a reduction to make sure all processes have the result data
  MPI_Reduce(all_junction_data, all_junction_data, num_junctions, JUNCTION_DATA, Sync_Junction_Data, 0, MPI_COMM_WORLD);
  MPI_Reduce(all_road_data, all_road_data, num_roads, ROAD_DATA, Sync_Road_Data, 0, MPI_COMM_WORLD);

  // map the reduced result data back into the roadmap 
  if(rank == 0){
    for(int i=0;i<num_junctions;i++){
      for(int k=0; k<num_junctions; k++){
        if(roadMap[i].id == all_junction_data[k].junction_id){
          roadMap[i].total_number_vehicles = all_junction_data[k].num_vehicles_on_junction;
          roadMap[i].total_number_crashes = all_junction_data[k].crashed_vehicles_on_junction;
          break;
        }
      }
      
      for(int j=0;j<roadMap[i].num_roads;j++){
        for(int k=0; k<num_roads; k++){
          if(roadMap[i].roads[j].id == all_road_data[k].road_id){
            roadMap[i].roads[j].total_number_vehicles = all_road_data[k].num_vehicles_on_road;
            roadMap[i].roads[j].max_concurrent_vehicles = all_road_data[k].max_concurrent_vehicles_on_road;
            break;
          }
        }
      }
    }
  }
  
  // free any allocated data, derived datatypes and finalize MPI
  free(all_road_data);
  free(all_junction_data);
  mpiFree();
  mpiTeardown();

  if(rank == 0){
    // On termination display a final summary and write detailed information to file
    printf(
      "Finished after %d mins: %d vehicles, %d passengers delivered, %d passengers stranded, %d crashed vehicles, %d vehicles exhausted fuel\n",
      MAX_MINS, final_data.total_vehicles, final_data.passengers_delivered, final_data.passengers_stranded, 
      final_data.vehicles_crashed, final_data.vehicles_exhausted_fuel
    );

    printf("Elapsed Time: %f", MPI_Wtime() - start_time);
    writeDetailedInfo();
  }
  return 0;
}


// sets up necessary MPI calls before executing the simulation
// in this case we use it to set up derived datatypes
static void mpiSetup(){

  MPI_Type_contiguous(5, MPI_INT, &DATA);
  MPI_Type_commit(&DATA);
  MPI_Op_create(&globalSyncData, 1, &Sync_Data);

  int items = 4;
	int blocklengths[4] = {1,1,1,1};

	MPI_Datatype types[4] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT};
  struct VehicleData vehicle_data;

  MPI_Aint main_addr, junction_count_addr, junction_id_addr, r_id_addr;
	MPI_Get_address(&vehicle_data, &main_addr);
	MPI_Get_address(&vehicle_data.num_vehicles_on_junction, &junction_count_addr);
	MPI_Get_address(&vehicle_data.junction_id, &junction_id_addr);
	MPI_Get_address(&vehicle_data.road_id, &r_id_addr);

  // find the exact offset required to get to the data address inside the message structure.
	MPI_Aint offsets[6] = {
    0, 
    junction_count_addr - main_addr, 
    junction_id_addr - main_addr,
    r_id_addr - main_addr,
  };

	MPI_Type_create_struct(items, blocklengths, offsets, types, &VEHICLE_DATA);
	MPI_Type_commit(&VEHICLE_DATA);
  MPI_Op_create(&globalSyncVehicleData, 1, &Sync_Vehicle_Data);

  items = 3;
	int junc_blocklengths[3] = {1,1,1};

	MPI_Datatype junc_types[3] = {MPI_INT, MPI_INT, MPI_INT};
  
  struct JunctionData junction_data;

  MPI_Aint junc_main_addr, junc_id_addr, junc_num_vehicles_addr, junc_crashed_vehicles_addr;
	MPI_Get_address(&junction_data, &junc_main_addr);
  MPI_Get_address(&junction_data.junction_id, &junc_id_addr);
	MPI_Get_address(&junction_data.num_vehicles_on_junction, &junc_num_vehicles_addr);
	MPI_Get_address(&junction_data.crashed_vehicles_on_junction, &junc_crashed_vehicles_addr);

  // find the exact offset required to get to the data address inside the message structure.
	MPI_Aint junc_offsets[3] = {
    junc_id_addr - junc_main_addr, 
    junc_num_vehicles_addr - junc_main_addr,
    junc_crashed_vehicles_addr - junc_main_addr
  };

	MPI_Type_create_struct(items, junc_blocklengths, junc_offsets, junc_types, &JUNCTION_DATA);
	MPI_Type_commit(&JUNCTION_DATA);
  MPI_Op_create(&globalSyncJunctionData, 1, &Sync_Junction_Data);

  items = 3;
	int road_blocklengths[3] = {1,1,1};

	MPI_Datatype road_types[3] = {MPI_INT, MPI_INT, MPI_INT};
  struct RoadData road_data;

  MPI_Aint road_main_addr, road_num_vehicle_addr, road_id_addr, road_max_concurrent_addr;
	MPI_Get_address(&road_data, &road_main_addr);
  MPI_Get_address(&road_data.num_vehicles_on_road, &road_num_vehicle_addr);
	MPI_Get_address(&road_data.road_id, &road_id_addr);
	MPI_Get_address(&road_data.max_concurrent_vehicles_on_road, &road_max_concurrent_addr);

  // find the exact offset required to get to the data address inside the message structure.
	MPI_Aint road_offsets[3] = {
    road_num_vehicle_addr - road_main_addr, 
    road_id_addr - road_main_addr,
    road_max_concurrent_addr - road_main_addr
  };

	MPI_Type_create_struct(items, road_blocklengths, road_offsets, road_types, &ROAD_DATA);
	MPI_Type_commit(&ROAD_DATA);
  MPI_Op_create(&globalSyncRoadData, 1, &Sync_Road_Data);

}


// free any derived datatypes
static void mpiFree(){
  MPI_Type_free(&DATA);
  MPI_Type_free(&VEHICLE_DATA);
  MPI_Type_free(&JUNCTION_DATA);
  MPI_Type_free(&ROAD_DATA);
}


// a task mapping defined by the user, this will map user created functions to task ids
// this mapping allows for starting processes functions defined within the mapping.
void taskMapping(enum Task type, int count){

  // any generic required error handling can be added here
  if(type >= task_count){
    fprintf(stderr, "Unknown task %d supplied\n", type);
    mpiFree();
    mpiTeardown();
  }

  if(type == initialize_vehicles) {
    initVehicles(count);
  }
  else if(type == generate_vehicles) {
    for(int i=0; i<count; i++){
      activateRandomVehicle();
    }
  }
  else if(type == update_roads){
    updateRoad();
  }
  else if(type == update_vehicles){
    updateVehicle();
  }
  else if(type == synchronize_data){
    struct Data temp;
    MPI_Reduce(&globalData, &temp, 1, DATA, Sync_Data, 0, new_communicator);
  }
}


// code to update the roads, mostly left unchanged from provided code.  
static void updateRoad(){
  for(int i=0;i<num_junctions;i++){

    if(roadMap[i].hasTrafficLights && roadMap[i].num_roads > 0){
      // Each minute of simulation time we enable a new road to be taken
      roadMap[i].trafficLightsRoadEnabled=elapsed_mins % roadMap[i].num_roads;
    }

    for(int j=0;j<roadMap[i].num_roads;j++){
      struct RoadStruct * road=&roadMap[i].roads[j];
      int num_vehicles_on_road=0;
      
      for(int k=0;k<MAX_VEHICLES;k++){
        if (vehicles[k].active && vehicles[k].roadOn == road){
          num_vehicles_on_road++;
        }
      }

      int total_vehicles = num_vehicles_on_road;
      // perform a reduction call here, this ensures that when we calculate the current speed of the 
      // road we use the global vehicles on the road, not just the vehicles present for the running process.
      MPI_Allreduce(&num_vehicles_on_road, &total_vehicles, 1, MPI_INT, MPI_SUM, new_communicator);

      // Recalculate current speed based on congestion and length (minus from max speed)
      road->currentSpeed=road->maxSpeed - total_vehicles;

      if(road->currentSpeed < 10){
        road->currentSpeed=10;
      }
    }
  }
}


// code to update vehicles, left mostly unchanged from provided code.
static void updateVehicle(){
 
  // locally compute vehicle data
  for(int i=0; i<MAX_VEHICLES; i++){

    if(vehicles[i].roadOn != NULL){
      vehicle_data[i].num_vehicles_on_road = vehicles[i].roadOn->numVehiclesOnRoad;
      vehicle_data[i].road_id = vehicles[i].roadOn->id;
    }

    if(vehicles[i].currentJunction != NULL){
      vehicle_data[i].num_vehicles_on_junction = vehicles[i].currentJunction->num_vehicles;
      vehicle_data[i].junction_id = vehicles[i].currentJunction->id;
    }
  }
  
  // a reduction to make sure all processes have data regarding the global number of vehicles on roads & junctions
  MPI_Allreduce(&vehicle_data, &vehicle_data, 1, VEHICLE_DATA, Sync_Vehicle_Data, new_communicator);
 
  for(int i=0;i<MAX_VEHICLES;i++){
    if(vehicles[i].active){
      handleVehicleUpdate(i);
    }
  }
}


/**
 * Handles an update for a specific vehicle that is on a road or waiting at a junction
 **/
static void handleVehicleUpdate(int i){

  // we track the vehicle changes made on the local process
  int local_junction_vehicle_changes = 0;
  int local_road_vehicle_changes = 0;

  if(getCurrentSeconds() - vehicles[i].start_t > vehicles[i].fuel){
    globalData.vehicles_exhausted_fuel++;
    globalData.passengers_stranded+=vehicles[i].passengers;
    if(vehicles[i].currentJunction != NULL){
      vehicles[i].currentJunction->num_vehicles--;
      local_junction_vehicle_changes--; 
      vehicles[i].currentJunction=NULL;
    }
    if(vehicles[i].roadOn != NULL){
      vehicles[i].roadOn->numVehiclesOnRoad--;
      local_road_vehicle_changes--;
      vehicles[i].roadOn=NULL;
    }
    vehicles[i].active=0;
    return;
  }

  if(vehicles[i].roadOn != NULL && vehicles[i].currentJunction == NULL){

    // Means that the vehicle is currently on a road
    time_t sec=getCurrentSeconds();
    int latest_time=sec - vehicles[i].last_distance_check_secs;

    if(latest_time < 1){
      return;
    }

    vehicles[i].last_distance_check_secs=sec;
    double travelled_length=latest_time * vehicles[i].speed;
    vehicles[i].remaining_distance-=travelled_length;

    if(vehicles[i].remaining_distance <= 0){
      // Left the road and arrived at the target junction
      vehicles[i].arrived_road_time=0;
      vehicles[i].last_distance_check_secs=0;
      vehicles[i].remaining_distance=0;
      vehicles[i].speed=0;
      vehicles[i].currentJunction=vehicles[i].roadOn->to;
      vehicles[i].currentJunction->num_vehicles++;
      local_junction_vehicle_changes++;
      vehicles[i].currentJunction->total_number_vehicles++;
      vehicles[i].roadOn->numVehiclesOnRoad--;
      local_road_vehicle_changes++;
      vehicles[i].roadOn=NULL;
    }
  }
 
  if(vehicles[i].currentJunction != NULL){
    if(vehicles[i].roadOn == NULL){

      // If the road is NULL then the vehicle is on a junction and not on a road
      if(vehicles[i].currentJunction->id == vehicles[i].dest){
        // Arrived! Job done!
        globalData.passengers_delivered+=vehicles[i].passengers;
        vehicles[i].active=0;
      } 

      else{
        // Plan route from here
        int next_junction_target=planRoute(vehicles[i].currentJunction->id, vehicles[i].dest);

        if(next_junction_target != -1){
          int road_to_take=findAppropriateRoad(next_junction_target, vehicles[i].currentJunction);
          assert(vehicles[i].currentJunction->roads[road_to_take].to->id == next_junction_target);
          vehicles[i].roadOn=&(vehicles[i].currentJunction->roads[road_to_take]);
          vehicles[i].roadOn->numVehiclesOnRoad++;
          local_road_vehicle_changes++;
          vehicles[i].roadOn->total_number_vehicles++;


          // use the earlier global vehicle data we synced and account for the local process
          // vehicle changes as well when performing this check.
          vehicle_data[i].num_vehicles_on_road += local_road_vehicle_changes;
          if(vehicles[i].roadOn->max_concurrent_vehicles < vehicle_data[i].num_vehicles_on_road){
            vehicles[i].roadOn->max_concurrent_vehicles=vehicle_data[i].num_vehicles_on_road;
          }

          vehicles[i].remaining_distance=vehicles[i].roadOn->roadLength;
          vehicles[i].speed=vehicles[i].roadOn->currentSpeed;
          if(vehicles[i].speed > vehicles[i].maxSpeed){
            vehicles[i].speed=vehicles[i].maxSpeed;
          }
        } 
        else{
          // Report error (this should never happen)
          fprintf(stderr, "No longer a viable route\n");
          exit(-1);
        }
      }
    }

    // Here we have selected a junction, now it's time to determine if the vehicle can be released from the junction
    char take_road=0;
   
    if(vehicles[i].currentJunction->hasTrafficLights){
      // Need to check that we can go, otherwise need to wait until road enabled by traffic light
      take_road=vehicles[i].roadOn != &vehicles[i].currentJunction->roads[vehicles[i].currentJunction->trafficLightsRoadEnabled];
    } 
    else{
      // If not traffic light then there is a chance of collision. We utilize the global vehicle data and account for
      // local changes to calculate the crash rate.
      int collision=getRandomInteger(0,8)*(vehicle_data[i].num_vehicles_on_junction + local_junction_vehicle_changes);
      
      if(collision > 40){
        // Vehicle has crashed!
        globalData.passengers_stranded+=vehicles[i].passengers;
        globalData.vehicles_crashed++;
        vehicles[i].active=0;
        vehicles[i].currentJunction->total_number_crashes++;
      }
      take_road=1;
    }

    // If take the road then clear the junction
    if (take_road){
      vehicles[i].last_distance_check_secs=getCurrentSeconds();
      vehicles[i].currentJunction->num_vehicles--;
      local_junction_vehicle_changes--;
      vehicles[i].currentJunction = NULL;
    }
  }
}


/**
 * Finds the road index out of the junction's roads that leads to a specific
 * destination junction
 **/
static int findAppropriateRoad(int dest_junction, struct JunctionStruct * junction) {
  for(int j=0;j<junction->num_roads;j++){
    if (junction->roads[j].to->id == dest_junction){
      return j;
    }
  }
  return -1;
}


/**
 * Plans a route from the source to destination junction, returning the junction after
 * the source junction. This will be called to plan a route from A (where the vehicle
 * is currently) to B (the destination), so will return the junction that most be travelled
 * to next. -1 is returned if no route is found
 **/
static int planRoute(int source_id, int dest_id) {
  
  if(VERBOSE_ROUTE_PLANNER){
    printf("Search for route from %d to %d\n", source_id, dest_id);
  }

  double * dist=(double*) malloc(sizeof(double) * num_junctions);
  char * active=(char*) malloc(sizeof(char) * num_junctions);
  struct JunctionStruct ** prev=(struct JunctionStruct **) malloc(sizeof( struct JunctionStruct *) * num_junctions);

  int activeJunctions=num_junctions;

  for(int i=0;i<num_junctions;i++){
    active[i]=1;
    prev[i]=NULL;
    if(i != source_id){
      dist[i]=LARGE_NUM;
    }
  }

  dist[source_id]=0;

  while(activeJunctions > 0){
    int v_idx=findIndexOfMinimum(dist, active);

    if(v_idx == dest_id){
      break;
    }

    struct JunctionStruct *v=&roadMap[v_idx];
    active[v_idx]=0;
    activeJunctions--;

    for(int i=0;i<v->num_roads;i++){
      if(active[v->roads[i].to->id] && dist[v_idx] != LARGE_NUM){
        double alt=dist[v_idx] + v->roads[i].roadLength / (v->id == source_id ? v->roads[i].currentSpeed : v->roads[i].maxSpeed);

        if(alt < dist[v->roads[i].to->id]){
          dist[v->roads[i].to->id]=alt;
          prev[v->roads[i].to->id]=v;
        }
      }
    }
  }

  free(dist);
  free(active);
  int u_idx=dest_id;
  int * route=(int*) malloc(sizeof(int) * num_junctions);
  int route_len=0;
  if(prev[u_idx] != NULL || u_idx == source_id){
    
    if (VERBOSE_ROUTE_PLANNER){
      printf("Start at %d\n", u_idx);
    } 

    while(prev[u_idx] != NULL){
      route[route_len]=u_idx;
      u_idx=prev[u_idx]->id;
      
      if (VERBOSE_ROUTE_PLANNER){
        printf("Route %d\n", u_idx);  
      } 

      route_len++;
    }
  }

  free(prev);

  if(route_len > 0){
    int next_jnct=route[route_len-1];
    
    if(VERBOSE_ROUTE_PLANNER){
      printf("Found next junction is %d\n", next_jnct);
    } 

    free(route);
    return next_jnct;
  }

  if(VERBOSE_ROUTE_PLANNER){
    printf("Failed to find route between %d and %d\n", source_id, dest_id);
  } 

  free(route);
  return -1;
}


/**
 * Finds the index of the input array that is active and has the smallest number
 **/
static int findIndexOfMinimum(double * dist, char * active){
  double min_dist=LARGE_NUM+1;
  int current_min=-1;

  for(int i=0;i<num_junctions;i++){
    if(active[i] && dist[i] < min_dist){
      min_dist=dist[i];
      current_min=i;
    }
  }

  return current_min;
}


/**
 * Initialises vehicles at the start of the simulation and will activate a
 * specified number
 **/
static void initVehicles(int num_initial){

  vehicles=(struct VehicleStruct*) malloc(sizeof(struct VehicleStruct) * MAX_VEHICLES);
  for(int i=0;i<MAX_VEHICLES;i++){
    vehicles[i].active=0;
    vehicles[i].roadOn=NULL;
    vehicles[i].currentJunction=NULL;
    vehicles[i].maxSpeed = 0;
  }
 
  for(int i=0;i<num_initial;i++){
    activateRandomVehicle();
  }
}


/**
 * Activates a vehicle and sets its type and route randomly
 **/
static int activateRandomVehicle(){

  int random_vehicle_type=getRandomInteger(0, 5);
  enum VehicleType vehicleType;

  if(random_vehicle_type == 0){
    vehicleType=BUS;
  } 
  else if(random_vehicle_type == 1){
    vehicleType=CAR;
  } 
  else if(random_vehicle_type == 2){
    vehicleType=MINI_BUS;
  } 
  else if(random_vehicle_type == 3){
    vehicleType=COACH;
  } 
  else if(random_vehicle_type == 4){
    vehicleType=MOTORBIKE;
  } 
  else if(random_vehicle_type == 5){
    vehicleType=BIKE;
  }

  return activateVehicle(vehicleType);
}


/**
 * Activates a vehicle with a specific type, will find an idle vehicle data
 * element and then initialise this with a random (but valid) route between
 * two junction. The new vehicle's index is returned,
 * or -1 if there are no free slots.
 **/
static int activateVehicle(enum VehicleType vehicleType){

  int id=findFreeVehicle();

  if(id >= 0){
    globalData.total_vehicles++;
    vehicles[id].active=1;
    vehicles[id].start_t=getCurrentSeconds();
    vehicles[id].last_distance_check_secs=0;
    vehicles[id].speed=0;
    vehicles[id].remaining_distance=0;
    vehicles[id].arrived_road_time=0;
    vehicles[id].source=vehicles[id].dest=getRandomInteger(0, num_junctions);

    while(vehicles[id].dest == vehicles[id].source){
      // Ensure that the source and destination are different
      vehicles[id].dest=getRandomInteger(0, num_junctions);

      if(vehicles[id].dest != vehicles[id].source){
        // See if there is a viable route between the source and destination
        int next_jnct=planRoute(vehicles[id].source, vehicles[id].dest);

        if(next_jnct == -1){
          // Regenerate source and dest
          vehicles[id].source=vehicles[id].dest=getRandomInteger(0, num_junctions);
        }
      }
    }

    vehicles[id].currentJunction=&roadMap[vehicles[id].source];
    vehicles[id].currentJunction->num_vehicles++;
    vehicles[id].currentJunction->total_number_vehicles++;
    vehicles[id].roadOn=NULL;

    if(vehicleType == CAR){
      vehicles[id].maxSpeed=CAR_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, CAR_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(CAR_MIN_FUEL, CAR_MAX_FUEL);
    } 
    else if(vehicleType == BUS){
      vehicles[id].maxSpeed=BUS_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, BUS_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(BUS_MIN_FUEL, BUS_MAX_FUEL);
    } 
    else if(vehicleType == MINI_BUS){
      vehicles[id].maxSpeed=MINI_BUS_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, MINI_BUS_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(MINI_BUS_MIN_FUEL, MINI_BUS_MAX_FUEL);
    } 
    else if(vehicleType == COACH){
      vehicles[id].maxSpeed=COACH_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, COACH_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(COACH_MIN_FUEL, COACH_MAX_FUEL);
    } 
    else if(vehicleType == MOTORBIKE){
      vehicles[id].maxSpeed=MOTOR_BIKE_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, MOTOR_BIKE_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(MOTOR_BIKE_MIN_FUEL, MOTOR_BIKE_MAX_FUEL);
    } 
    else if(vehicleType == BIKE){
      vehicles[id].maxSpeed=BIKE_MAX_SPEED;
      vehicles[id].passengers=getRandomInteger(1, BIKE_PASSENGERS);
      vehicles[id].fuel=getRandomInteger(BIKE_MIN_FUEL, BIKE_MAX_FUEL);
    } 
    else{
      fprintf(stderr, "Unknown vehicle type\n");
    }
    return id;
  }
  return -1;
}


/**
 * Iterates through the vehicles array and finds the first free entry, returns
 * the index of this or -1 if none is found
 **/
static int findFreeVehicle(){
  for(int i=0;i<MAX_VEHICLES/(sim_size + 1);i++){
    if(!vehicles[i].active){
      return i;
    }
  }
  return -1;
}


/**
 * Parses the provided roadmap file and uses this to build the graph of
 * junctions and roads, as well as reading traffic light information
 **/
static void loadRoadMap(char * filename){
  
  enum ReadMode currentMode=NONE;
  char buffer[MAX_ROAD_LEN];
  FILE * f=fopen(filename, "r");

  if(f == NULL){
    fprintf(stderr, "Error opening roadmap file '%s'\n", filename);
    exit(-1);
  }
  int road_id = 0;
  while(fgets(buffer, MAX_ROAD_LEN, f)){

    if (buffer[0]=='%'){
      continue;
    }

    if(buffer[0]=='#'){
      if(strncmp("# Road layout:", buffer, 14)==0){
        char * s=strstr(buffer, ":");
        num_junctions=atoi(&s[1]);
        roadMap=(struct JunctionStruct*) malloc(sizeof(struct JunctionStruct) * num_junctions);

        for(int i=0;i<num_junctions;i++){
          roadMap[i].id=i;
          roadMap[i].num_roads=0;
          roadMap[i].num_vehicles=0;
          roadMap[i].hasTrafficLights=0;
          roadMap[i].total_number_crashes=0;
          roadMap[i].total_number_vehicles=0;
          // Not ideal to allocate all roads size here
          roadMap[i].roads=(struct RoadStruct*) malloc(sizeof(struct RoadStruct) * MAX_NUM_ROADS_PER_JUNCTION);
        }

        currentMode=ROADMAP;
      }

      if(strncmp("# Traffic lights:", buffer, 17)==0) {
        currentMode=TRAFFICLIGHTS;
      }
    } 
    else{
      if(currentMode == ROADMAP){
        char * space=strstr(buffer, " ");
        *space='\0';

        int from_id=atoi(buffer);
        char * nextspace=strstr(&space[1], " ");
        *nextspace='\0';

        int to_id=atoi(&space[1]);
        char * nextspace2=strstr(&nextspace[1], " ");
        *nextspace='\0';

        int roadlength=atoi(&nextspace[1]);
        int speed=atoi(&nextspace2[1]);

        if(roadMap[from_id].num_roads >= MAX_NUM_ROADS_PER_JUNCTION){
          fprintf(stderr, "Error: Tried to create road %d at junction %d, but maximum number of roads is %d, increase 'MAX_NUM_ROADS_PER_JUNCTION'",
            roadMap[from_id].num_roads, from_id, MAX_NUM_ROADS_PER_JUNCTION);
          exit(-1);
        }

        roadMap[from_id].roads[roadMap[from_id].num_roads].from=&roadMap[from_id];
        roadMap[from_id].roads[roadMap[from_id].num_roads].to=&roadMap[to_id];
        roadMap[from_id].roads[roadMap[from_id].num_roads].roadLength=roadlength;
        roadMap[from_id].roads[roadMap[from_id].num_roads].maxSpeed=speed;
        roadMap[from_id].roads[roadMap[from_id].num_roads].numVehiclesOnRoad=0;
        roadMap[from_id].roads[roadMap[from_id].num_roads].currentSpeed=speed;
        roadMap[from_id].roads[roadMap[from_id].num_roads].total_number_vehicles=0;
        roadMap[from_id].roads[roadMap[from_id].num_roads].max_concurrent_vehicles=0;
        roadMap[from_id].roads[roadMap[from_id].num_roads].id=road_id;
        roadMap[from_id].num_roads++;
        num_roads++;
        road_id++;
      } 
      else if (currentMode == TRAFFICLIGHTS){
        int id=atoi(buffer);

        if(roadMap[id].num_roads > 0){
          roadMap[id].hasTrafficLights=1;
        }
      }
    }
  }

  fclose(f);
}


/**
 * Generates a random integer between two values, including the from value up to the to value minus
 * one, i.e. from=0, to=100 will generate a random integer between 0 and 99 inclusive
 **/
static int getRandomInteger(int from, int to){
  return (rand() % (to-from)) + from;
}


/**
 * Writes out to file detailed information (at termination) around junction and road metrics
 **/
static void writeDetailedInfo(){

  FILE * f=fopen("results", "w");

  for(int i=0;i<num_junctions;i++){
    fprintf(f, "Junction %d: %d total vehicles and %d crashes\n", i, roadMap[i].total_number_vehicles, roadMap[i].total_number_crashes);

    for(int j=0;j<roadMap[i].num_roads;j++){
      fprintf(f, "--> Road from %d to %d: Total vehicles %d and %d maximum concurrently\n", roadMap[i].roads[j].from->id,
        roadMap[i].roads[j].to->id, roadMap[i].roads[j].total_number_vehicles, roadMap[i].roads[j].max_concurrent_vehicles);
    }
  }
  fclose(f);
}


/**
 * Retrieves the current time in seconds
 **/
static time_t getCurrentSeconds(){
  struct timeval curr_time;
  gettimeofday(&curr_time, NULL);
  time_t current_seconds=curr_time.tv_sec;
  return current_seconds;
}
