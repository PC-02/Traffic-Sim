#ifndef TASK_ENUM_H
#define TASK_ENUM_H

#define VERBOSE_ROUTE_PLANNER 0
#define LARGE_NUM 99999999.0

#define MAX_ROAD_LEN 100
#define MAX_VEHICLES 1000
#define MAX_MINS 10
#define MIN_LENGTH_SECONDS 2
#define MAX_NUM_ROADS_PER_JUNCTION 50
#define SUMMARY_FREQUENCY 5
#define INITIAL_VEHICLES 100

#define BUS_PASSENGERS 80
#define BUS_MAX_SPEED 50
#define BUS_MIN_FUEL 10
#define BUS_MAX_FUEL 100
#define CAR_PASSENGERS 4
#define CAR_MAX_SPEED 100
#define CAR_MIN_FUEL 1
#define CAR_MAX_FUEL 40
#define MINI_BUS_MAX_SPEED 80
#define MINI_BUS_PASSENGERS 15
#define MINI_BUS_MIN_FUEL 2
#define MINI_BUS_MAX_FUEL 75
#define COACH_MAX_SPEED 60
#define COACH_PASSENGERS 40
#define COACH_MIN_FUEL 20
#define COACH_MAX_FUEL 200
#define MOTOR_BIKE_MAX_SPEED 120
#define MOTOR_BIKE_PASSENGERS 2
#define MOTOR_BIKE_MIN_FUEL 1
#define MOTOR_BIKE_MAX_FUEL 20
#define BIKE_MAX_SPEED 10
#define BIKE_PASSENGERS 1
#define BIKE_MIN_FUEL 2
#define BIKE_MAX_FUEL 10
#define DEBUG_MODE 0
 
 
// this should be user defined and will help with the mapping of functions
enum Task {
    initialize_vehicles=0,
    generate_vehicles=1,
    update_roads=2,
    update_vehicles=3,
    synchronize_data=4,
    task_count=5,    // To get the number of tasks
};

// this is used for logging tasks
extern char* task_descriptions[task_count];


void taskMapping(enum Task, int);

#endif 