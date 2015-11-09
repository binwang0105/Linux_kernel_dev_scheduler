#ifndef WRRINFO_H
#define WRRINFO_H

#define MAX_CPUS 4
struct wrr_info {
	int num_cpus;
	int nr_running[MAX_CPUS];
	int total_weight[MAX_CPUS];
};

#endif
