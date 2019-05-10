#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "memory.h"

int main(const int argc, char * const argv[]){
	struct msgbuf msg;

	int id = atoi(argv[1]);

	key_t key = ftok("memory.h", 1234);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	const int sid = shmget(key, sizeof(struct shared_memory), 0);
	if(sid == -1){
		perror("shmget");
		return -1;
	}

	void * mem = (struct shared_memory*) shmat(sid, NULL, 0);
	if(mem == (void*)-1){
		perror("shmat");
		return -1;
	}

	key = ftok("memory.h", 5678);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	const int mid = msgget(key, 0);
	if(mid == -1){
		perror("msgget");
		return -1;
	}

	srand(id);

	msg.addr = 0;
	while(msg.addr >= 0){

		msg.mtype 		= NP;
		msg.id  			= id + 1;	//IDs are 0 based, but we can't send message with type 0
		msg.reference	= ((rand() % 100) < 55) ? 0 : 1;
		msg.addr			= (rand() % PROCESS_PAGES) * 1024;	//random address

		if(	(msgsnd(mid, (void*)&msg, MESSAGE_SIZE, 0) < 0) ||
				(msgrcv(mid, (void*)&msg, MESSAGE_SIZE, msg.id, 0) < 0)	){
			perror("msg");
			break;
		}
		//We don't check for terminate, because parent will give us msg.addr = -1
	}

	shmdt(mem);
	return 0;
}
