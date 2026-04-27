#!/bin/bash
#SBATCH --job-name=mikeneuder-task3
#SBATCH -N 1
#SBATCH --ntasks-per-node=1 --cpus-per-task=1
#SBATCH --mem=128G
#SBATCH --time=02:00:00             # total run time limit (HH:MM:SS)
#SBATCH --mail-type=begin,end,fail  # receive email notifications
#SBATCH --mail-user=mn3265@cs.princeton.edu
#SBATCH --output=/scratch/network/mn3265/COS568-LI-SP26/test.out
#SBATCH --error=/scratch/network/mn3265/COS568-LI-SP26/test.err

echo 'Your job is running on node(s):'
echo $SLURM_NODELIST

lscpu | grep -E "L1d|L1i|L2|L3"