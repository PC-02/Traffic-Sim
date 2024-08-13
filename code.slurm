#!/bin/bash

#SBATCH --nodes=2
#SBATCH --time=15:00
#SBATCH --partition=standard
#SBATCH --qos=short
#SBATCH --tasks-per-node=36
#SBATCH --cpus-per-task=1
#SBATCH --exclusive

# Replace [budget code] below with your budget code
#SBATCH --account=[budget code]

module load mpt/2.25

srun -n 64 ./bin/code ./inputs/medium_problem

