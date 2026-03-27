#!/bin/bash
#SBATCH --job-name=mikeneuder-task1
#SBATCH -N 1
#SBATCH --ntasks-per-node=1 --cpus-per-task=1
#SBATCH --mem=16G
#SBATCH --time=02:00:00             # total run time limit (HH:MM:SS)
#SBATCH --mail-type=begin,end,fail  # receive email notifications
#SBATCH --mail-user=mn3265@cs.princeton.edu
#SBATCH --output=/scratch/network/mn3265/task1.out
#SBATCH --error=/scratch/network/mn3265/task1.err

echo 'Your job is running on node(s):'
echo $SLURM_NODELIST

module load anaconda3/2023.3

cd /scratch/network/mn3265/COS568-LI-SP26
bash ./scripts/run_benchmarks.sh