#include <inc/mmu.h>
#include <inc/types.h>
#include <inc/string.h>
#include <inc/x86.h>
#include <inc/memlayout.h>
#include <kernel/task.h>
#include <kernel/mem.h>

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
struct Segdesc gdt[6] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W , 0x0, 0xffffffff, 3),

	// First TSS descriptors (starting from GD_TSS0) are initialized
	// in task_init()
	[GD_TSS0 >> 3] = SEG_NULL
	
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};



static struct tss_struct tss;
Task tasks[NR_TASKS];

extern char bootstack[];

extern char UTEXT_start[], UTEXT_end[];
extern char UDATA_start[], UDATA_end[];
extern char UBSS_start[], UBSS_end[];
extern char URODATA_start[], URODATA_end[];
/* Initialized by task_init */
uint32_t UTEXT_SZ;
uint32_t UDATA_SZ;
uint32_t UBSS_SZ;
uint32_t URODATA_SZ;

Task *cur_task = NULL; //Current running task

extern void sched_yield(void);

int sys_getpid()
{
	if(cur_task)
		return cur_task->task_id;
	else
		return -1;
}

int sys_sleep(uint32_t time)
{
	if(cur_task != NULL && cur_task->state == TASK_RUNNING){
		cur_task->state = TASK_SLEEP;
		cur_task->remind_ticks = (int32_t) time;
		sched_yield();
		return 0;
	}
	else{
		panic("Can't sleep the task\n");
		return -1;
	}
}

/* TODO: Lab5
 * 1. Find a free task structure for the new task,
 *    the global task list is in the array "tasks".
 *    You should find task that is in the state "TASK_FREE"
 *    If cannot find one, return -1.
 *
 * 2. Setup the page directory for the new task
 *
 * 3. Setup the user stack for the new task, you can use
 *    page_alloc() and page_insert(), noted that the va
 *    of user stack is started at USTACKTOP and grows down
 *    to USR_STACK_SIZE, remember that the permission of
 *    those pages should include PTE_U
 *
 * 4. Setup the Trapframe for the new task
 *    We've done this for you, please make sure you
 *    understand the code.
 *
 * 5. Setup the task related data structure
 *    You should fill in task_id, state, parent_id,
 *    and its schedule time quantum (remind_ticks).
 *
 * 6. Return the pid of the newly created task.
 *
 */
int task_create()
{
	Task *ts = NULL;
	int i, id;
	struct PageInfo *ppage;

	/* Find a free task structure */
	for(i = 0; i < NR_TASKS; i++)
	{
		if(tasks[i].state == TASK_FREE || tasks[i].state == TASK_STOP)
			break;
	}

	if(i >= NR_TASKS)
		return -1;

	ts = &tasks[i];
	id = i;

  /* Setup Page Directory and pages for kernel*/
  if (!(ts->pgdir = setupkvm()))
    panic("Not enough memory for per process page directory!\n");

	printk("pgdir num = %x\n", PADDR(ts->pgdir));

  /* Setup User Stack */
	for(i = USTACKTOP - USR_STACK_SIZE; i < USTACKTOP; i += PGSIZE){
		ppage = page_alloc(1);	
		if(ppage != NULL)
			page_insert(ts->pgdir, ppage, (void *) i, PTE_U | PTE_W);
		else
			return -1;
	}

	/* Setup Trapframe */
	memset( &(ts->tf), 0, sizeof(ts->tf));

	ts->tf.tf_cs = GD_UT | 0x03;
	ts->tf.tf_ds = GD_UD | 0x03;
	ts->tf.tf_es = GD_UD | 0x03;
	ts->tf.tf_ss = GD_UD | 0x03;
	ts->tf.tf_esp = USTACKTOP-PGSIZE;

	/* Setup task structure (task_id and parent_id) */
	ts->task_id = id;
	if(cur_task != NULL)
		ts->parent_id = cur_task->task_id;
	else
		ts->parent_id = -1;

	ts->remind_ticks = TIME_QUANT;
	ts->state = TASK_RUNNABLE;

	return ts->task_id;
}


/* TODO: Lab5
 * This function free the memory allocated by kernel.
 *
 * 1. Be sure to change the page directory to kernel's page
 *    directory to avoid page fault when removing the page
 *    table entry.
 *    You can change the current directory with lcr3 provided
 *    in inc/x86.h
 *
 * 2. You have to remove pages of USER STACK
 *
 * 3. You have to remove pages of page table
 *
 * 4. You have to remove pages of page directory
 *
 * HINT: You can refer to page_remove, ptable_remove, and pgdir_remove
 */
static void task_free(int pid)
{
	Task *ts = &tasks[pid];
	int i;

	if(pid > 0 && pid < NR_TASKS){
		// change the page directory to kernel's page directory
		lcr3(PADDR(kern_pgdir));	

		// remove pages of USER STACK
		for(i = USTACKTOP - USR_STACK_SIZE; i < USTACKTOP; i += PGSIZE)
			page_remove(ts->pgdir, (void *) i);

		ptable_remove(ts->pgdir);
		pgdir_remove(ts->pgdir);
	}
}

void sys_kill(int pid)
{
	if (pid > 0 && pid < NR_TASKS)
	{
	/* TODO: Lab 5
   * Remember to change the state of tasks
   * Free the memory
   * and invoke the scheduler for yield
   */
		if(tasks[pid].state == TASK_FREE || tasks[pid].state == TASK_STOP)
			return;
		tasks[pid].state = TASK_STOP;
		task_free(pid);	
		sched_yield();
	}
}

/* TODO: Lab 5
 * In this function, you have several things todo
 *
 * 1. Use task_create() to create an empty task, return -1
 *    if cannot create a new one.
 *
 * 2. Copy the trap frame of the parent to the child
 *
 * 3. Copy the content of the old stack to the new one,
 *    you can use memcpy to do the job. Remember all the
 *    address you use should be virtual address.
 *
 * 4. Setup virtual memory mapping of the user prgram 
 *    in the new task's page table.
 *    According to linker script, you can determine where
 *    is the user program. We've done this part for you,
 *    but you should understand how it works.
 *
 * 5. The very important step is to let child and 
 *    parent be distinguishable!
 *
 * HINT: You should understand how system call return
 * it's return value.
 */
int sys_fork()
{
  /* pid for newly created process */
  int pid, i;
	struct PageInfo *ppageNew;
	uint32_t kvaNew;
	Task *childTask;

	if((uint32_t) cur_task){
		// create an empty task
		if(!(pid = task_create()))
			return -1;

		childTask = &(tasks[pid]);

		// copy the Trapframe
		memcpy(&(childTask->tf), &(cur_task->tf), sizeof(struct Trapframe));

		// copy stack
		for(i = USTACKTOP - USR_STACK_SIZE; i < USTACKTOP; i += PGSIZE){
			ppageNew = page_lookup(childTask->pgdir, (void *) i, NULL);	
			if(ppageNew != NULL){
				kvaNew = (uint32_t) page2kva(ppageNew);
				memcpy(kvaNew, i, PGSIZE);
			}
			else
				return -1;
		}

		/* Step 4: All user program use the same code for now */
		setupvm(childTask->pgdir, (uint32_t)UTEXT_start, UTEXT_SZ);
		setupvm(childTask->pgdir, (uint32_t)UDATA_start, UDATA_SZ);
		setupvm(childTask->pgdir, (uint32_t)UBSS_start, UBSS_SZ);
		setupvm(childTask->pgdir, (uint32_t)URODATA_start, URODATA_SZ);

		// clear eax for child, so fork will return 0 in child
		childTask->tf.tf_regs.reg_eax = 0;

		return pid;
	}
	else
		return -1;
}

/* TODO: Lab5
 * We've done the initialization for you,
 * please make sure you understand the code.
 */
void task_init()
{
  extern int user_entry();
	int i;
  UTEXT_SZ = (uint32_t)(UTEXT_end - UTEXT_start);
  UDATA_SZ = (uint32_t)(UDATA_end - UDATA_start);
  UBSS_SZ = (uint32_t)(UBSS_end - UBSS_start);
  URODATA_SZ = (uint32_t)(URODATA_end - URODATA_start);

	/* Initial task sturcture */
	for (i = 0; i < NR_TASKS; i++)
	{
		memset(&(tasks[i]), 0, sizeof(Task));
		tasks[i].state = TASK_FREE;
	}
	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	memset(&(tss), 0, sizeof(tss));
	tss.ts_esp0 = (uint32_t)bootstack + KSTKSIZE;
	tss.ts_ss0 = GD_KD;

	// fs and gs stay in user data segment
	tss.ts_fs = GD_UD | 0x03;
	tss.ts_gs = GD_UD | 0x03;

	/* Setup TSS in GDT */
	gdt[GD_TSS0 >> 3] = SEG16(STS_T32A, (uint32_t)(&tss), sizeof(struct tss_struct), 0);
	gdt[GD_TSS0 >> 3].sd_s = 0;

	/* Setup first task */
	i = task_create();
	cur_task = &(tasks[i]);
	printk("first task id = %d parent id = %d\n", cur_task->task_id, 
				 cur_task->parent_id);

  /* For user program */
  setupvm(cur_task->pgdir, (uint32_t)UTEXT_start, UTEXT_SZ);
  setupvm(cur_task->pgdir, (uint32_t)UDATA_start, UDATA_SZ);
  setupvm(cur_task->pgdir, (uint32_t)UBSS_start, UBSS_SZ);
  setupvm(cur_task->pgdir, (uint32_t)URODATA_start, URODATA_SZ);
  cur_task->tf.tf_eip = (uint32_t)user_entry;
	
	/* Load GDT&LDT */
	lgdt(&gdt_pd);

	lldt(0);

	// Load the TSS selector 
	ltr(GD_TSS0);

	cur_task->state = TASK_RUNNING;
}
