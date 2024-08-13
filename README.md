# About

This is a traffic simulation code that I have parallelized as part of an assignment for my MSc. `init_code.c` describes the inital simulation code provided to us. The goal of this assignment was to parallelize the code so it can run smaller problem simulations faster and support running larger problem sizes. 

I parallelized this code using [MPI](https://en.wikipedia.org/wiki/Message_Passing_Interface), specifically by creating a generic parallel framework with [MPT](https://www.researchgate.net/publication/228983529_Message_passing_toolkit_MPT_software_on_XT3). This framework is generic & extensible and can be used for other software as well. Code detailing the framework itself can be found in `src/pool.c` and code detailing changes made to the inital simulation to utilize the parallel framework are in `src/code.c`.

# Running the program

```
make
mpirun -np 3 ./bin/code ./inputs/tiny_problem
```

The above can be used for any inputs in the directory

# Using the framework

To utilize the framework a `code.h` file must be defined in the include folder. This header **must** contatin an enum like below,


```
enum Task {
    ...
    task_count=NUM,    // To get the number of tasks
};

// this is used for logging tasks
extern char* task_descriptions[task_count];

void taskMapping(enum Task, int);
```

where `...` represents all the tasks to be done, `task_count` represents the number of tasks added. The `task_desciptions` is utilized for debugging and logs. The `taskMapping` function is used to run the provided tasks. The code of this function should be defined in the main code file.


# Running the serial code

```
make init
./init_code ./inputs/tiny_problem
```

Similar to the above it can be run for any input in the folder.


# Compiling details

This code has been ran and tested on Cirrus, it is run using the mpicc (gcc 8.4.1 20200928) compiler with the mpt/2.25 module loaded.