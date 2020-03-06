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
#include <limits.h>

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

enum cmd{START_ROUND = 0, END_ROUND = 1, MOVE_TO = 2, START_MOVING = 3};

struct piece {
	char type;
	int value;
	pid_t owner;
	int x,y;
};

struct Pair {
	int x, y;
};

struct self_pawns{
	pid_t pid;
	int x, y;
};

struct Pair place_random();

void handle_term(int signal);
void find_flags(struct Pair *flags);
struct Pair min_dist_flag(struct self_pawns pawn);
int dist_flag(struct Pair flags, struct self_pawns pawn);

struct piece *board;
struct sembuf sem_board;
struct self_pawns *self_pawns;
struct Pair *flags;

int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G;

char *args[] = {"./pawn", NULL};

int sem_board_id, shm_id, msg_id;
int  num_flags;

int main(int argc, char * argv[], char** envp){

	int i;
	long rcv, r;
	struct Pair move;

	pid_t child_pid;

	struct msgbuf msg_queue;
	struct Pair pair;
	struct sigaction sa;
	sigset_t my_mask;


	key_t msg_key;

	FILE *shm_file;
	FILE *sem_board_file;
	FILE *msgid_file;

	sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

	SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_BASE = atoi(getenv("SO_BASE"));
	SO_NUM_P = atoi(getenv("SO_NUM_P"));
	SO_NUM_G = atoi(getenv("SO_NUM_G"));

	self_pawns = (struct self_pawns*)malloc(sizeof(struct self_pawns)*SO_NUM_P);

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

	printf("hey sono il player %d.\n", getpid());

	rcv = (long) getpid();

	for(i = 0; i< SO_NUM_P; i++){
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
		sem_board.sem_op = -1;
		sem_board.sem_num= SO_BASE*SO_ALTEZZA;
		semop(sem_board_id, &sem_board, 1);
		switch(child_pid = fork()){
			case -1:
				TEST_ERROR;
				exit(EXIT_FAILURE);
			case 0:
				execve(args[0], args, envp);
				break;
			default:
				break;
		}
		self_pawns[i].pid = child_pid;
		pair = place_random();
		self_pawns[i].x = pair.x;
		self_pawns[i].y = pair.y;
		msg_queue.mtype = (long)(self_pawns[i].pid);
		sprintf(msg_queue.mtext, "%d %d", self_pawns[i].x, self_pawns[i].y);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
		msg_queue.mtype = (long) getppid();
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}

	rcv = (long) getpid();
	for(;;){
		if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
			switch (atoi(msg_queue.mtext)) {
				case START_ROUND:
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					num_flags = atoi(msg_queue.mtext);
					flags = (struct Pair*)malloc(sizeof(struct Pair)*num_flags);
					find_flags(flags);
					for(i = 0; i < SO_NUM_P; i++){
						move = min_dist_flag(self_pawns[i]);
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d", MOVE_TO);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d %d", move.x, move.y);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}
					printf("CAN MOVE?\n");
					msg_queue.mtype = (long) getppid();
					msgsnd(msg_id, &msg_queue, LENGTH, 0);
					break;
				case END_ROUND:
					break;
				case START_MOVING:
					for(i = 0; i < SO_NUM_P; i++){
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d", START_MOVING);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}
					printf("SAID MOVE\n");
					break;
				default:
					break;
			}
		}
	}
	exit(EXIT_SUCCESS);
}

/**
 * [place_random description]
 * @return [description]
 */
struct Pair place_random(){
	int ret_val;
	int r;
	int x, y;
	struct Pair pair;

	srand(time(0));
	while(1){
		x = rand() % SO_BASE;
		y = rand() % SO_ALTEZZA;
		r = y * SO_BASE + x;
		if(r >= SO_BASE * SO_ALTEZZA) continue;
		sem_board.sem_op = -1;
		sem_board.sem_num= r;
		sem_board.sem_flg = IPC_NOWAIT;
		ret_val = semop(sem_board_id, &sem_board, 1);
		if(ret_val == -1){
			if (errno == EAGAIN) continue;
			else TEST_ERROR;
		}else break;
	}
	board[r].type = 'p';
	board[r].owner = getpid();
	pair.x = x;
	pair.y = y;
	return pair;
}

/**
 * [handle_term description]
 * @param signal [description]
 */
void handle_term(int signal){
	int i, status;
	pid_t child_pid;

	for(i = 0; i< SO_NUM_P; i++){
		kill(self_pawns[i].pid, SIGTERM);
	}
	while ((child_pid = wait(&status)) != -1) {
		printf("terminated pawns process...\n");
	}
	if (errno == ECHILD) {
		free(self_pawns);
		free(flags);
		exit(EXIT_SUCCESS);
	} else {
		fprintf(stderr, "Error #%d: %s\n", errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

/**
 * [find_flags description]
 * @param flags [description]
 */
void find_flags(struct Pair *flags){
	int i,j;
	for(i = 0, j = 0; i < SO_BASE * SO_ALTEZZA; i++){
		if(board[i].type == 'f'){
			flags[j].x = board[i].x;
			flags[j].y = board[i].y;
			j++;
		}
	}
}

/**
 * [min_dist_flag description]
 * @param  pawn [description]
 * @return      [description]
 */
struct Pair min_dist_flag(struct self_pawns pawn){
	int i, index, dist, min = INT_MAX;
	struct Pair p;
	for(i = 0; i < num_flags; i++){
		dist = dist_flag(flags[i], pawn);
		if(dist < min) {
			min = dist;
			index = i;
		}
	}
	p.x = flags[index].x;
	p.y = flags[index].y;
	return p;
}

/**
 * [dist_flag description]
 * @param  flag [description]
 * @param  pawn [description]
 * @return      [description]
 */
int dist_flag(struct Pair flag, struct self_pawns pawn){
	int dist_x, dist_y;

	dist_x = flag.x - pawn.x;
	dist_y = flag.y - pawn.y;
	return abs(dist_x) + abs(dist_y);
}
