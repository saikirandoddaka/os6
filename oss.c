#include <sys/shm.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "memory.h"

struct suspended_users{
	int id;			           //user identifier
	int addr;		          //address
	struct timespec t;	  //time this request will be satisfied
};

#define MAX_NS 1000000000

int sid = -1, mid = -1; //memory and message queue identifiers
struct shared_memory *mem = NULL;  //shared memory region
unsigned int N = NP;
unsigned int oss_interrupted = 0;

struct suspended_users suspended[NP];
unsigned int suspended_head = 0, suspended_end = 0;

enum counters {SUSPENDED, REFERENCES, FAULTS, SWAPS, WRITES, READS, NUM_COUNTERS};
unsigned int counter[NUM_COUNTERS];

void send_stop_msg(){
  int i;
  struct msgbuf msg;

  msg.addr = -1;  //addr = - 1, breaks the user loop

  for(i=0; i < NP; i++){
  	if(mem->users[i].pid > 0){
      msg.mtype = mem->users[i].id + 1;
  		if(msgsnd(mid, (void*)&msg, MESSAGE_SIZE, 0) == -1){
  			perror("msgsnd");
  			return;
  		}
    }
  }
}


void term_handler(int sig, siginfo_t *si, void *unused){

  switch(sig){
    case SIGTERM:
      printf("OSS Caught SIGTERM at %li:%li\n", mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
    case SIGINT:
      printf("OSS Caught SIGINT at %li:%li\n", mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
      break;
    default:
      printf("OSS Caught signal at %li:%li\n", mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
      break;
  }

  oss_interrupted = 1; //stop master loop
  send_stop_msg();
}


void print_frame_info(){
  int i;

  printf("Current memory layout at time %li:%li is:\n", mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
  printf("\t\t\tOccupied\t\tRefByte\t\tDirtyBit\n");

  for(i=0; i < FRAME_COUNT; i++){
    char * label = (mem->frame_tbl[i].id >= 0) ? "Yes" : "No";
    printf("Frame %d: %s\t\t\t%3d\t\t\t\t\t%d\n", i, label, mem->frame_tbl[i].refbyte, mem->frame_tbl[i].dirty);
  }
}

void oss_results(){

  fprintf(stderr, "### Memory Simulation Details ###\n");
  fprintf(stderr, "References (read, write, suspended): %u, %u, %u\n", counter[READS], counter[WRITES], counter[SUSPENDED]);
  fprintf(stderr, "Actions (faults, swaps): %u, %u\n",counter[FAULTS], counter[SWAPS]);

  fprintf(stderr, "Access per sec: %f\n", (float) counter[REFERENCES] / (float)mem->oss_clock.tv_sec);
  fprintf(stderr, "Faults per access: %f\n", (float) counter[FAULTS] / (float)counter[REFERENCES]);
}

void tsadd(struct timespec* x, struct timespec* y){
  x->tv_sec  += y->tv_sec;
	x->tv_nsec += y->tv_nsec;
	if(x->tv_nsec > MAX_NS){
		x->tv_sec++;
		x->tv_nsec -= MAX_NS;
	}
}

void fr_clear(struct frame *fr){
  fr->page = fr->id = -1;
  fr->dirty = fr->refbyte = 0;
}

int fr_unused(struct frame * tbl){
  int i;
  for(i=0; i < FRAME_COUNT; i++){
    if(tbl[i].id == -1)
      return i;
  }
  return -1;
}

//Find the "youngest" frame, which is the one with lowest refbyte
int fr_youngest(struct frame * tbl){
  int i, oldest=0;
  for(i=0; i < FRAME_COUNT; i++){
    if(tbl[i].refbyte < tbl[oldest].refbyte){
      oldest = i;
    }
  }
  return oldest;
}

void fr_age(struct frame * tbl){
  int i;
  for(i=0; i < FRAME_COUNT; i++){
    tbl[i].refbyte = tbl[i].refbyte >> 1;
  }
}

void fr_ref(struct frame *tbl, const int i){

  static int num_refs = 0;

  if(++num_refs >= 10){
    fr_age(tbl);
    num_refs = 0;
  }

  tbl[i].refbyte = (1 << 7);  
}

void fr_dirty(const int frame){
  struct frame *fr = &mem->frame_tbl[frame];

  if(fr->dirty){  //if its dirty

    printf("Master: Dirty bit of frame %d set, adding additional time to the clock\n", frame);
    struct timespec dirty_time = {.tv_sec = 0, .tv_nsec = 20};
    tsadd(&mem->oss_clock, &dirty_time);	//add 15ns to clock, for disk read/write
  }
  fr->dirty = 1;  //we have wrote data to the frame, set it dirty
}

void p_clear(struct page * p){

  if(p->frame >= 0)
    fr_clear(&mem->frame_tbl[p->frame]);

  p->frame = -1;
}

int p_swap(const int id, const int page){
  int frame = fr_youngest(mem->frame_tbl);  //swap oldest frame
  struct frame* fr = &mem->frame_tbl[frame];

  struct page * p = &mem->users[fr->id].page_tbl[fr->page];
  if(p->frame < 0){
	  fprintf(stderr, "Error: empty page\n");
	  term_handler(SIGTERM, NULL, NULL);
  }

  const int old_id = mem->users[fr->id].id;

  printf("Master: Clearing frame %d and swapping P%d page %d for P%d page %d\n", p->frame, old_id, fr->page, id, page);
  counter[SWAPS]++;

  frame = p->frame;
  p_clear(p);

  return frame;
}

void u_clear(struct user * u){
	int i;
	for(i=0; i < PROCESS_PAGES; i++)
      p_clear(&u->page_tbl[i]);

  u->pid = u->id = -1;
}

int u_unused(struct user * users){
  int i;
  for(i=0; i < N; i++){
    if(mem->users[i].id < 0){
      return i;
    }
  }
  return N;
}

void oss_stop(){
  printf("OSS: Finished at time %lu:%lu\n", mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);

  oss_results();

  int i, status;
  for(i=0; i < NP; i++){
  	if(mem->users[i].pid > 0)
      wait(&status);
  }

  shmdt(mem);
  shmctl(sid, IPC_RMID, NULL);
  msgctl(mid, IPC_RMID, NULL);
}

int init(){
	const key_t key = ftok("memory.h", 1234);
	if(key == -1){
		perror("ftok");
		return EXIT_FAILURE;
	}

	sid = shmget(key, sizeof(struct shared_memory), IPC_CREAT | IPC_EXCL | S_IRWXU);
	if(sid == -1){
		perror("shmget");
		return EXIT_FAILURE;
	}

	mem = (struct shared_memory*) shmat(sid, NULL, 0);
  if(mem == (void*)-1){
    perror("shmat");
    return EXIT_FAILURE;
  }

  
  bzero(mem, sizeof(struct shared_memory));

  key_t msg_key = ftok("memory.h", 5678);
	if(msg_key == -1){
		perror("ftok");
		return EXIT_FAILURE;
	}

	mid = msgget(msg_key, IPC_CREAT | IPC_EXCL | 0666);
	if(mid == -1){
		perror("msgget");
		return EXIT_FAILURE;
	}

  bzero(counter, sizeof(int)*NUM_COUNTERS);

	//clear page table and suspended
  int i;
	for(i=0; i < NP; i++){
		u_clear(&mem->users[i]);
    suspended[i].id = -1;
  }

	//clear frame table
	for(i=0; i < FRAME_COUNT; i++)
		fr_clear(&mem->frame_tbl[i]);

  srand(time(NULL));

  return EXIT_SUCCESS;
}

int oss_fork(){

  struct sigaction sa;
  char id_arg[10];

  //search for a free user slot
  int i = u_unused(mem->users);
  if(i == N)
    return 0;

  struct user *pinfo = &mem->users[i];
  pinfo->id = i;
  snprintf(id_arg, sizeof(id_arg), "%d", pinfo->id);

  pid_t pid = fork();
  switch(pid){

    case -1:
      perror("fork");
      return -1;
      break;

    case 0: //child process

      //clear the signal handler from child
      sa.sa_flags = 0;
      sigemptyset(&sa.sa_mask);
      sa.sa_handler = SIG_DFL;
      if( (sigaction(SIGTERM, &sa, NULL) == -1) ||
          (sigaction(SIGINT, &sa, NULL) == -1) ){
        perror("sigaction");
        return EXIT_FAILURE;
      }

      execl("./user", "./user", id_arg, NULL);  //run the user program
      perror("execl");
      exit(EXIT_FAILURE);
      break;

    default:
      pinfo->pid = pid;
      printf("OSS: Generating process with PID %u at time %lu:%lu\n", pinfo->id, mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
      break;
  }

  return 0;
}

int oss_clock_increment(){
  struct timespec maxTimeBetweenNewProcs = {.tv_nsec = 500, .tv_sec = 1};
  struct timespec tick = {.tv_sec = 1, .tv_nsec = 1000};  //oss clock tick
  struct timespec fork_at = {.tv_sec = 0, .tv_nsec = 0};  //time we have to fork

  tsadd(&mem->oss_clock, &tick);  //advance clock

  
  if( (mem->oss_clock.tv_sec >= fork_at.tv_sec) ||
      ( (mem->oss_clock.tv_sec  == fork_at.tv_sec) &&
        (mem->oss_clock.tv_nsec >= fork_at.tv_nsec))  ){

    //generate next fork time
    fork_at.tv_sec = rand() % maxTimeBetweenNewProcs.tv_sec;
    fork_at.tv_nsec = rand() % maxTimeBetweenNewProcs.tv_nsec;
    tsadd(&fork_at, &mem->oss_clock);

    oss_fork();  //make a new process
  }

  return 0;
}

int mem_reference(const int id, const int p, const int addr){
  int rv;

  if(p > PROCESS_PAGES){
		fprintf(stderr, "Error: Page index > %d\n", PROCESS_PAGES);
		return -1;
	}

  struct user * pinfo = &mem->users[id];

  struct page * page = &pinfo->page_tbl[p];

  if(page->frame == -1){	//if page has no frame assigned

    printf("Master: Address %d is not in a frame, pagefault\n", addr);
    counter[FAULTS]++;

    page->frame = fr_unused(mem->frame_tbl);  //find frame that is not used
    if(page->frame >= 0){  //if found one
      printf("Master: Using free frame %d for P%d page %d\n", page->frame, id, p);
    }else{
      page->frame = p_swap(pinfo->id, p);
    }

    struct frame * fr = &mem->frame_tbl[page->frame];

    fr_clear(fr);
  	fr->page = p;
  	fr->id = id;

    counter[SUSPENDED]++;

    //add process id to suspended list
    suspended[suspended_end].id = id;
    suspended[suspended_end].addr = addr;
    suspended[suspended_end].t.tv_sec = 0;
    suspended[suspended_end].t.tv_nsec = 15;
    tsadd(&suspended[suspended_end].t, &mem->oss_clock);	//add 15ns to clock, for disk read/write
    suspended_end = (suspended_end + 1) % NP;

    rv = 0; //0 is return to avoid sending message reply

  }else{ //no page fault

    //no loading, just add time for accessing the page
    struct timespec access_time = {.tv_sec = 0, .tv_nsec = 10};
    tsadd(&mem->oss_clock, &access_time);	//add 10ns to clock, for memory read/write
    rv = 1;
  }

  fr_ref(mem->frame_tbl, page->frame);  //update frame reference byte

  return rv;
}

int oss_read(const int id, const int addr){

  counter[READS]++;
  printf("Master: P%d requesting read of address %d at time %li:%li\n", id, addr, mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);

	const int p = addr / (PAGE_SIZE*1024);  //get the page index

  int rv = mem_reference(id, p, addr); //load it into memory
	if(rv > 0){
    struct user * u = &mem->users[id];
    printf("Master: Address %d in frame %d, giving data to P%d at time %li:%li\n", addr, u->page_tbl[p].frame, id, mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
	}

  return rv;
}

int oss_write(const int id, const int addr){

  counter[WRITES]++;

  printf("Master: P%d requesting write of address %d at time %li:%li\n", id, addr, mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
	const int page     = addr / (PAGE_SIZE*1024);

  int rv = mem_reference(id, page, addr);
	if(rv > 0){
    struct user * u = &mem->users[id];
    const int frame = u->page_tbl[page].frame;
    printf("Master: Address %d in frame %d, writing data to frame at time %li:%li\n", addr, frame, mem->oss_clock.tv_sec, mem->oss_clock.tv_nsec);
    fr_dirty(frame);
	}
	return rv;
}

//Receive memory requests from users and process them
int user_requests(){
	struct msgbuf msg;

  if( msgrcv(mid, (void*)&msg, MESSAGE_SIZE, NP, 0) == -1){
		perror("msgrcv");
		return -1;
	}

	//process request
  int rv = -1;
  switch(msg.reference){
    case 0: rv = oss_read(msg.id-1, msg.addr);  break;
    case 1: rv = oss_write(msg.id-1, msg.addr); break;
    default:
      fprintf(stderr, "Error: Invalid reference %i\n", msg.reference);
      break;
  }

	if(rv != 0){
		msg.mtype = msg.id;
		msg.addr = rv;  //user address as return value
		if(msgsnd(mid, (void*)&msg, MESSAGE_SIZE, 0) == -1){
			perror("msgsnd");
			return -1;
		}
	}

	return 0;
}

//Check queue of pages, waiting to be loaded from device
int suspended_requests(){
	struct msgbuf msg;

	//check dev q, and unblock all request before current time

	while(suspended[suspended_head].id >= 0){

		struct user * pinfo = &mem->users[suspended[suspended_head].id];

		//page is loaded
		msg.mtype = pinfo->id + 1;  //IDs are 0 based, but we can't send message with type 0
		msg.addr = 1;

    //tell process
		if(msgsnd(mid, (void*)&msg, MESSAGE_SIZE, 0) == -1){
			perror("msgsnd");
			return -1;
		}

    printf("Master: Indicating to P%d that write has happened to address %d\n", pinfo->id, suspended[suspended_head].addr);

    suspended[suspended_head].id = -1;  //clear the entry
    suspended_head = (suspended_head + 1) % NP;
	}

	return 0;
}

static void limit_log_size(){
	static int log_disabled = 0;	//set to 1, if log is over 10 Mb
	struct stat st;

	if(log_disabled)
		return;

	if(stat("oss.txt", &st) == -1){
		perror("stat");
		return;
	}

	if(st.st_size > 10*1024*1024){	//if log is larger that 10MB
		stdout = freopen("/dev/null", "w", stdout);
		log_disabled = 1;
	}
}

//simulate a memory oss_memory
void oss_memory(){

  while(oss_interrupted == 0){ //loop until all procs are done
    oss_clock_increment();

    user_requests();
    suspended_requests();

    if((++counter[REFERENCES] % 100) == 0){  //on each 100 memory accesses
  		print_frame_info();
			limit_log_size();
		}
  }

  oss_stop();
}

int main(const int argc, char * const argv[]){

  struct sigaction sa;

  if(argc != 2){
    fprintf(stderr, "Error: You have to enter max running processes\n");
    return EXIT_FAILURE;
  }

  N = atoi(argv[1]);
  if(N < 0){
    fprintf(stderr, "Error: Invalid parameter value %d\n", N);
    return EXIT_FAILURE;
  }

  if(N > NP){
    N = NP;
  }

  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = term_handler;
  if( (sigaction(SIGTERM, &sa, NULL) == -1) ||
      (sigaction(SIGINT, &sa, NULL) == -1) ){
    perror("sigaction");
    return EXIT_FAILURE;
  }

  stdout = freopen("oss.txt", "w", stdout);

  if(init() == EXIT_FAILURE)
    return EXIT_FAILURE;

  oss_memory();

  return EXIT_SUCCESS;
}
