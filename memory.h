#include <sys/types.h>

//Number of running processes
#define NP 18

//values are in KB
#define PAGE_SIZE	1
#define MEMORY_SIZE 	256
#define FRAME_COUNT MEMORY_SIZE
#define PROCESS_PAGES	32
#define MESSAGE_SIZE 3*sizeof(int)

struct page
{
	int frame;	//frame used by the page
};

struct frame
{
	int page;		//page using the frame
	int id;			//frame user ID
	int dirty;

	unsigned char refbyte;
};

struct msgbuf
{
	long mtype;			//id of sender/receiver - master uses NP, users their ID

	int id;					
	int reference;	//1 for read, 0 for write reference
	int addr;				
};

struct user
{
	int pid;
	int id;

	struct page	  page_tbl[PROCESS_PAGES];		//page table
};

struct shared_memory
{
	struct user users[NP];
	struct frame  frame_tbl[FRAME_COUNT];	//frame table
	struct timespec oss_clock;
};

void frame_clear(struct frame *fr);
void frame_set_ref(struct frame *fr, const int i);
int find_unused(struct frame * frames);
int find_lowest_refbyte(struct frame * frames);
void show_frames(struct frame * frames, const int sec, const int nsec);
