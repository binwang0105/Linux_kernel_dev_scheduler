#include "sched.h"

#include <linux/slab.h>


#define FG_W	10
#define BG_W	1



#ifdef CONFIG_CGROUP_SCHED
static char group_path[PATH_MAX];
static char *task_group_path(struct task_group *tg)
{
	if (autogroup_path(tg, group_path, PATH_MAX))
		return group_path;
	if (!tg->css.cgroup) {
		group_path[0] = '\0';
		return group_path;
	}
	cgroup_path(tg->css.cgroup, group_path, PATH_MAX);
	return group_path;
}
#endif



void init_wrr_rq(struct wrr_rq *wrr_rq)
{
	INIT_LIST_HEAD(&wrr_rq->list);
	wrr_rq->rq_weight = 0;
	wrr_rq->wrr_nr_running = 0;
}



static void enqueue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &(p->wrr);
	
	if (flags & ENQUEUE_WAKEUP)
		wrr_se->timeout = 0;
	
	struct list_head *list = &rq->wrr.list;
	
	if (flags & ENQUEUE_HEAD)
		list_add(&wrr_se->run_list, list);
	else
		list_add_tail(&wrr_se->run_list, list);
	
	++rq->wrr.wrr_nr_running;
	
	inc_nr_running(rq);
}

static void update_curr_wrr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	if (curr->sched_class != &wrr_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	 curr->se.sum_exec_runtime += delta_exec;
	 account_group_exec_runtime(curr, delta_exec);

	 curr->se.exec_start = rq->clock_task;
	 cpuacct_charge(curr, delta_exec);
}


static void dequeue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	update_curr_wrr(rq);
	list_del_init(&wrr_se->run_list);
	--rq->wrr.wrr_nr_running;
	
	dec_nr_running(rq);
}


static void requeue_task_wrr(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_wrr_entity *wrr_se = &p->wrr;
	
	struct list_head *list = &rq->wrr.list;
	if (head)
		list_move(&wrr_se->run_list, list);
	else
		list_move_tail(&wrr_se->run_list, list);
	
}

static void yield_task_wrr(struct rq *rq)
{
	requeue_task_wrr(rq, rq->curr, 0);
}

static void check_preempt_curr_wrr(struct rq *rq, struct task_struct *p, int flags)
{
	return;
}

static struct task_struct *pick_next_task_wrr(struct rq *rq)
{
	struct sched_wrr_entity *first;
	struct task_struct *p;
	struct wrr_rq *wrr_rq;
	
	wrr_rq = &rq->wrr;
	
	if (!wrr_rq->wrr_nr_running)
		return NULL;
	
	if(list_empty(&rq->wrr.list)){
		return NULL;
	}
	
	first = list_first_entry(&rq->wrr.list, struct sched_wrr_entity, run_list);
	p = container_of(first, struct task_struct, wrr);
	if (p == NULL)
		return NULL;

	p->se.exec_start = rq->clock_task;
	
	return p;
}

static void put_prev_task_wrr(struct rq *rq, struct task_struct *p)
{
	update_curr_wrr(rq);
}


static int get_wrr_rq_weight(struct wrr_rq *wrr_rq)
{
	struct sched_wrr_entity *wrr_se;
	struct task_struct *tmp_p;
	char *gp;
	int result;
	
	wrr_rq->rq_weight = 0;
	list_for_each_entry(wrr_se,wrr_rq,wrr_rq){
		
		tmp_p = container_of(wrr_se, struct task_struct, wrr);
		
		gp = task_group_path(task_group(tmp_p));
		if(strcmp(gp,"/") == 0){//foreground
			wrr_rq->rq_weight += FG_W * WRR_TIMESLICE;
			
		}else if(strcmp(gp,"/bg_non_interactive") == 0){//background
			wrr_rq->rq_weight += BG_W * WRR_TIMESLICE;
		}else{
			wrr_rq->rq_weight += BG_W * WRR_TIMESLICE;
		}
		
	}
	return wrr_rq->rq_weight;

}


static int select_task_rq_wrr(struct task_struct *p, int sd_flag, int flags)
{
	struct wrr_rq *wrr_rq;
	int target_cpu;
	int min_weight;
	int tmp_weight;
	unsigned long tmp_cpu;
	
	
	target_cpu = task_cpu(p);
	min_weight = get_wrr_rq_weight(&cpu_rq(tmp_cpu)->wrr);
	
	
	if (p->wrr.nr_cpus_allowed == 1)
		return target_cpu;
	
	rq = cpu_rq(cpu);
	
	rcu_read_lock();
	for_each_online_cpu(tmp_cpu) {
		wrr_rq = &cpu_rq(tmp_cpu)->wrr;
		
		tmp_weight = get_wrr_rq_weight(wrr_rq);
		if(tmp_weight < min_weight){
			min_weight = tmp_weight;
			target_cpu = tmp_cpu;
		}
	}
	rcu_read_unlock();
	
	
	return target_cpu;

}


static void set_curr_task_wrr(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	p->se.exec_start = rq->clock_task;
	
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;
	
	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);
	
	if (soft != RLIM_INFINITY) {
		unsigned long next;
		
		p->wrr.timeout++;
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}


static void task_tick_wrr(struct rq *rq, struct task_struct *p, int queued)
{
	char *gp;
	struct sched_wrr_entity *wrr_se = &p->wrr;
	
	
	update_curr_wrr(rq);
	
	watchdog(rq, p);
	
	
	if (--p->wrr.time_slice)
		return;
	
	rcu_read_lock();
	gp = task_group_path(task_group(p));
	if(strcmp(gp,"/") == 0){//foreground
		p->wrr.time_slice = FG_W*WRR_TIMESLICE;
		
	}else if(strcmp(gp,"/bg_non_interactive") == 0){//background
		p->wrr.time_slice = BG_W * WRR_TIMESLICE;
	}else{
		p->wrr.time_slice = BG_W * WRR_TIMESLICE;
	}
	rcu_read_unlock();
	
	if(rq->wrr.list.prev != rq->wrr.list.next){
		requeue_task_wrr(rq,p,0);
		set_tsk_need_resched(p);
	}
	
}

static void prio_changed_wrr(struct rq *rq, struct task_struct *p, int oldprio)
{
	
}

static void switched_to_wrr(struct rq *rq, struct task_struct *p)
{
	if (p->on_rq && rq->curr != p)
		if (rq == task_rq(p))
			resched_task(rq->curr);
}


static unsigned int get_rr_interval_wrr(struct rq *rq, struct task_struct *task)
{
	char * gp;
	
	rcu_read_lock();
	gp = task_group_path(task_group(p));
	if(strcmp(gp,"/") == 0){//foreground
		return = FG_W*WRR_TIMESLICE;
		
	}else if(strcmp(gp,"/bg_non_interactive") == 0){//background
		return BG_W * WRR_TIMESLICE;
	}else{
		return = BG_W * WRR_TIMESLICE;
	}
	rcu_read_unlock();
}



const struct sched_class wrr_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_wrr,
	.dequeue_task		= dequeue_task_wrr,
	.yield_task		= yield_task_wrr,
	.check_preempt_curr	= check_preempt_curr_wrr,
	.pick_next_task		= pick_next_task_wrr,
	.put_prev_task		= put_prev_task_wrr,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_wrr,
#endif
	.set_curr_task          = set_curr_task_wrr,
	.task_tick		= task_tick_wrr,
	.prio_changed		= prio_changed_wrr,
	.switched_to		= switched_to_wrr,
	.get_rr_interval	= get_rr_interval_wrr,
};












