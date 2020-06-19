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

/*
	enumerations
 */
enum cmd{START_ROUND = 0, END_ROUND = 1, MOVE_TO = 2, START_MOVING = 3,
	 		CAUGHT = 4, FLAGS = 5, POSITION = 6};

/*
	structs
 */
struct piece {
	char type;
	int value;
	pid_t owner;
	int x,y;
};

struct Pair {
	int x, y;
};

struct Flag {
	int x, y;
};

struct pawn{
	pid_t pid;
	int x, y;
};

/*
	function prototypes
 */
void handle_term(int signal);
void handle_alarm(int signal);
void find_flags(struct Flag *flags);
void save_flags();
void free_all();
int dist_flag(struct Pair flags, struct pawn pawn);
struct Pair place_random();
struct Pair min_dist_flag(struct pawn pawn);

/*
	global variables
 */
struct piece *board;
struct sembuf sem_board;
struct pawn *self_pawns;
struct Flag *flags;
struct Pair *targets;

char *args[] = {"./pawn", NULL};

int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G;
int sem_board_id, shm_id, msg_id;
int num_flags;
int *flags_index;

int main(int argc, char * argv[], char** envp){

	int i, points, cmd, pawn_ind;
	long rcv, rcv_2, r;
	char * split_msg;
	struct Pair target;

	struct msgbuf msg_queue;

	pid_t child_pid;

	struct Pair pair;
	struct sigaction sa, saa;
	sigset_t my_mask, my_maskk;

	key_t msg_key;

	FILE *shm_file;
	FILE *sem_board_file;
	FILE *msgid_file;

	sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

	saa.sa_handler = &handle_alarm;
	saa.sa_flags = 0;
	sigemptyset(&my_maskk);
	saa.sa_mask = my_maskk;
	sigaction(SIGALRM, &saa, NULL);

	SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_BASE = atoi(getenv("SO_BASE"));
	SO_NUM_P = atoi(getenv("SO_NUM_P"));
	SO_NUM_G = atoi(getenv("SO_NUM_G"));

	self_pawns = (struct pawn*)malloc(sizeof(struct pawn)*SO_NUM_P);
	targets = (struct Pair*)malloc(sizeof(struct Pair)*SO_NUM_P);
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

	printf("[PLAYER %d] Joined the game.\n", getpid());
	printf("------------------------------------------\n");

	rcv = (long) getpid();

	/*
		generates all pawns, saves pawns pid and position,
		sends it to pawn, sends a msg to master
	 */
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

	/*
		waits for any message from the master
	 */
	for(;;){
		if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
			split_msg = strtok (msg_queue.mtext," ");
			cmd = atoi(split_msg);
			switch (cmd) {
				/*
					gets a msg with num_flags, finds all flag on the board,
					for all pawns sends MOVE_TO msg and coordinate (x,y)
					of the flag to be caught
				 */
				case START_ROUND:
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					num_flags = atoi(msg_queue.mtext);
					flags = (struct Flag*)malloc(sizeof(struct Flag)*num_flags);

					flags_index =  malloc(sizeof(int)*num_flags);
					for(i = 0; i < num_flags; i++){
						msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
						flags_index[i] = atoi(msg_queue.mtext);
					}
					save_flags();

					for(i = 0; i < SO_NUM_P; i++){
						target = min_dist_flag(self_pawns[i]);
						targets[i].x = target.x;
						targets[i].y = target.y;
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d", MOVE_TO);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d %d", target.x, target.y);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}
					msg_queue.mtype = (long) getppid();
					msgsnd(msg_id, &msg_queue, LENGTH, 0);
					break;
				/*

				 */
				case END_ROUND:
					free_all();
					break;
				/*
					for all pawns sends START_MOVING msg
				 */
				case START_MOVING:
					for(i = 0; i < SO_NUM_P; i++){
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d", START_MOVING);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}
					break;
				/*
					tells the master that a flag has been caught
				 */
				case CAUGHT:
					split_msg = strtok (NULL, " ");
					points =  atoi(split_msg);
					msg_queue.mtype = (long) getppid();
					sprintf(msg_queue.mtext, "%d %d %d", CAUGHT, getpid(), points);
					msgsnd(msg_id, &msg_queue, LENGTH, 0);
					break;

				default:
					break;
			}
		}
	}
	exit(EXIT_FAILURE);
}

/**
 * places a pawn in a random free spot on the board
 * @return coordinate (x,y) of the placed pawn
 */
struct Pair place_random(){
	int ret_val;
	int r;
	int x, y;
	struct Pair pair;

	srand(time(NULL));
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
 * handles SIGTERM signal, terminates all pawns and frees all structs
 * @param signal SIGTERM
 */
void handle_term(int signal){
	int i, status;
	pid_t child_pid;

	for(i = 0; i< SO_NUM_P; i++){
		kill(self_pawns[i].pid, SIGTERM);
	}

	while ((child_pid = wait(&status)) != -1) {
	}

	if (errno != ECHILD) {
		fprintf(stderr, "Error #%d: %s\n", errno, strerror(errno));
		free_all();
		exit(EXIT_FAILURE);
	}
	printf("[PLAYER %d] Logged out.\n", getpid());
	free_all();
	exit(EXIT_SUCCESS);
}

/**
 * for all pawns sends MOVE_TO msg
 * and coordinate (x,y) of the new target flag to be caught
 * @param signal SIGALRM
 */
void handle_alarm(int signal){
	struct msgbuf msg_queue1;
	long rcv_2;
	int i, pawn_ind, target_ind;
	struct Pair target;

	num_flags--;
	free(flags);
	flags = (struct Flag*)malloc(sizeof(struct Flag)*num_flags);
	save_flags();

	for(i = 0; i < SO_NUM_P; i++){
		target_ind = targets[i].y * SO_BASE + targets[i].x;
		if(board[target_ind].type != 'f'){
			msg_queue1.mtype = (long)(self_pawns[i].pid);
			sprintf(msg_queue1.mtext, "%d", POSITION);
			msgsnd(msg_id, &msg_queue1, LENGTH, 0);

			rcv_2 = (long) getpid() / POSITION;
			msgrcv(msg_id, &msg_queue1, LENGTH, rcv_2, 0);
			pawn_ind = atoi(msg_queue1.mtext);

			self_pawns[i].x = board[pawn_ind].x;
			self_pawns[i].y = board[pawn_ind].y;

			target = min_dist_flag(self_pawns[i]);
			targets[i].x = target.x;
			targets[i].y = target.y;
			msg_queue1.mtype = (long)(self_pawns[i].pid);
			sprintf(msg_queue1.mtext, "%d", MOVE_TO);
			msgsnd(msg_id, &msg_queue1, LENGTH, 0);
			msg_queue1.mtype = (long)(self_pawns[i].pid);
			sprintf(msg_queue1.mtext, "%d %d", target.x, target.y);
			msgsnd(msg_id, &msg_queue1, LENGTH, 0);
			msg_queue1.mtype = (long)(self_pawns[i].pid);
			sprintf(msg_queue1.mtext, "%d", START_MOVING);
			msgsnd(msg_id, &msg_queue1, LENGTH, 0);
		}
	}
}

/**
 * [save_flags description]
 */
void save_flags(){
	int i;

	for(i = 0; i < num_flags; i++){
		if(board[flags_index[i]].type = 'f'){
			flags[i].x = board[flags_index[i]].x;
			flags[i].y = board[flags_index[i]].y;
		}
	}
}

/**
 * gets the flag with minimum distance
 * @param  pawn pawn from which to calculate the distance
 * @return      coordinates (x,y) of the flag
 */
struct Pair min_dist_flag(struct pawn pawn){
	int i, index, dist, min = SO_BASE*SO_ALTEZZA + 1;
	struct Pair p, current_flag;

	for(i = 0; i < num_flags; i++){
		current_flag.x = flags[i].x;
		current_flag.y = flags[i].y;
		dist = dist_flag(current_flag, pawn);
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
 * gets the distance between a flag and a pawn
 * @param  flag flag
 * @param  pawn pawn
 * @return      distance in between
 */
int dist_flag(struct Pair flag, struct pawn pawn){
	int dist_x, dist_y;

	dist_x = flag.x - pawn.x;
	dist_y = flag.y - pawn.y;
	return abs(dist_x) + abs(dist_y);
}

/**
 * free all memory allocations
 */
void free_all(){
	free(self_pawns);
	free(flags);
	free(targets);
	free(flags_index);
}
