#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>

#define TEST_ERROR    if (errno) {fprintf(stderr, \
					   "%s:%d: PID=%5d: Error %d (%s)\n",\
					   __FILE__,\
					   __LINE__,\
					   getpid(),\
					   errno,\
					   strerror(errno));}

#define FILENAME_SHM  "shm_file.txt"
#define FILENAME_SEM_BOARD "sem_board_file.txt"
#define FILENAME_MSGID  "msgid_file.txt"
#define LENGTH 120

enum cmd{MOVE_TO = 2, START_MOVING = 3};

struct piece {
	char type;
	int value;
	int owner;
	int x,y;
	};

void handle_term(int signal);
void move_to(int go_to);

struct piece *board;
struct sembuf sem_board;

int SO_BASE;
int sem_board_id, shm_id, msg_id;
int self_x, self_y, self_ind;

int main(int argc, char * argv[], char** envp){

	long rcv;
	int go_to;

	struct msgbuf msg_queue;
	struct sigaction sa;
	sigset_t my_mask;

	key_t msg_key;

	FILE *shm_file;
	FILE *sem_board_file;
	FILE *msgid_file;

	char * split_msg;

	sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

	SO_BASE = atoi(getenv("SO_BASE"));

	shm_file = fopen(FILENAME_SHM, "r");
	fscanf(shm_file, "%d", &shm_id);
	fclose(shm_file);

	board = (struct piece*) shmat(shm_id, NULL, 0);

	sem_board_file = fopen(FILENAME_SEM_BOARD, "r");
	fscanf(sem_board_file, "%d", &sem_board_id);
	fclose(sem_board_file);

	msgid_file = fopen(FILENAME_MSGID, "r");
	fscanf(msgid_file, "%d", &msg_key);
	fclose(msgid_file);

	if (msg_key == IPC_PRIVATE) {
			fprintf(stderr, "Chiave %d errata\n", IPC_PRIVATE);
			return(-1);
		}
	msg_id = msgget(msg_key, 0600);
	if (msg_id == -1) TEST_ERROR;

	rcv = (long) getpid();
	if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
		split_msg = strtok (msg_queue.mtext," ");
		self_x = atoi(split_msg);
		split_msg = strtok (NULL, " ");
		self_y =  atoi(split_msg);
		self_ind = self_y * SO_BASE + self_x;
		board[self_ind].value = getpid();
	}
	for(;;){
		if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
			switch (atoi(msg_queue.mtext)) {
				case MOVE_TO:
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					go_to = atoi(msg_queue.mtext);
					move_to(go_to);
					printf("go to %d\n", go_to);
					break;
				default:
					break;
			}
		}
	}

	exit(EXIT_SUCCESS);
}

/**
 * [handle_term description]
 * @param signal [description]
 */
void handle_term(int signal){
	exit(EXIT_SUCCESS);
}

/**
 * [go_to description]
 * @param go_to [description]
 */
void move_to(int go_to){

}
