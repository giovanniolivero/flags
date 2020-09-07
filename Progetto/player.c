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
#include <sys/stat.h>
#include <fcntl.h>

#define TEST_ERROR    if (errno) {fprintf(stderr, \
					   "[PLAYER_ERROR] %s:%d: PID=%5d: Error %d (%s)\n",\
					   __FILE__,\
					   __LINE__,\
					   getpid(),\
					   errno,\
					   strerror(errno));}

#define FILENAME_INI  "./ipcs.ini"
#define LENGTH 500
#define BUF_SIZE 500

/*
	enumerations
 */
enum cmd{START_ROUND = 0, END_ROUND = 1, MOVE_TO = 2, START_MOVING = 3,
	 		CAUGHT = 4, ALRM = 5, POSITION = 6};

/*
	structs
 */
struct piece {
	char type;
	int value;
	pid_t owner;
	int x,y;
};

typedef struct Pawn{
	pid_t pid;
	int x, y;
}Pawn;

typedef struct Flag {
	int x, y;
	char available;
}Flag;

struct Pair {
	int x, y;
};

/*
	function prototypes
 */
void save_flags();
void update_flags();
void handle_alarm();
void handle_term(int signal);
struct Pair place_random();
struct Pair min_dist_flag(Pawn pawn);

/*
	global variables
 */
struct piece *board;
struct sembuf sem_board;
struct Flag *flags;
struct Pair *targets;

Pawn *self_pawns;

int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G;
int sem_board_id, shm_id, msg_id, prt_msg_id;
int num_flags;

int *flags_index;

char *args[] = {"./pawn", NULL};

int main(int argc, char * argv[], char** env){
    int i, cmd, points, caught_index;
	int current_moves = 0;
    long rcv;
    pid_t child_pid;

	char *line; /* line to write in ipcs.ini */
	char *split_msg; /* msg to split in rcv() */

	size_t len = 0;
    ssize_t read;

    struct Pair pair;
	struct Pair target;

    struct msgbuf msg_queue;
	struct msgbuf prt_msg_queue;

    struct sigaction sa;
	sigset_t my_mask;

	key_t msg_key;
	key_t prt_msg_key;

	FILE *ini_file;

    SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_BASE = atoi(getenv("SO_BASE"));
	SO_NUM_P = atoi(getenv("SO_NUM_P"));
	SO_NUM_G = atoi(getenv("SO_NUM_G"));

	/*
		sigaction init
	*/
    sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

    self_pawns = (Pawn*)malloc(sizeof(Pawn) * SO_NUM_P);
	targets = (struct Pair*)malloc(sizeof(struct Pair)*SO_NUM_P);
	line = malloc(sizeof(char)*100);
	bzero(line, 100);

	/*
		starts reading from ipcs.ini
	*/
	ini_file = fopen(FILENAME_INI, "r");

	if(ini_file == NULL) {
    	perror("Error opening file");
    	return(-1);
   	}

	/*
		gets semaphore id in ipcs.ini
	*/
   	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	sem_board_id = atoi(line);
		bzero(line, 100);
   	}

	/*
		gets shared memory id in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	shm_id = atoi(line);
		bzero(line, 100);
   	}

	board = (struct piece*) shmat(shm_id, NULL, 0);

	/*
		gets primary message key in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	msg_key = atoi(line);
		bzero(line, 100);
   	}
	if (msg_key == IPC_PRIVATE) {
		fprintf(stderr, "[PLAYER_ERROR] PLAYER %d wrong key %d\n", getpid(), IPC_PRIVATE);
		return(-1);
	}
	msg_id = msgget(msg_key, 0600);
	if (msg_id == -1) TEST_ERROR;

	/*
		gets priority message key in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	prt_msg_key = atoi(line);
   	}
	if (prt_msg_key == IPC_PRIVATE) {
		fprintf(stderr, "[PLAYER_ERROR] PLAYER %d wrong key %d\n", getpid(), IPC_PRIVATE);
		return(-1);
	}
	prt_msg_id = msgget(prt_msg_key, 0600);
	if (prt_msg_id == -1) TEST_ERROR;

	/*
		Closes ini_file and frees line allocation
	*/
	fclose(ini_file);
	free(line);

	printf("[PLAYER %d] Joined the game.\n", getpid());

    /*
		generates all pawns, saves pawns pid and position,
		sends it to pawn, sends a msg to master
	 */
    rcv = (long) getpid();
	for(i = 0; i < SO_NUM_P; i++){
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);

        sem_board.sem_op = -1;
		sem_board.sem_num= SO_BASE*SO_ALTEZZA;
		semop(sem_board_id, &sem_board, 1);

		switch(child_pid = fork()){
			case -1:
				TEST_ERROR;
				exit(EXIT_FAILURE);
			case 0:
				execve(args[0], args, env);
				break;
			default:
				break;
		}
		self_pawns[i].pid = child_pid;

		pair = place_random();
		self_pawns[i].x = pair.x;
		self_pawns[i].y = pair.y;

        /*
         	Sends to pawn the position
        */
		msg_queue.mtype = (long)(self_pawns[i].pid);
		sprintf(msg_queue.mtext, "%d %d", self_pawns[i].x, self_pawns[i].y);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);

		/*
            Sends to master confirm placement msg
        */
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

					/*
						Gets #flags
					*/
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					num_flags = atoi(msg_queue.mtext);
					flags = (Flag*)malloc(sizeof(Flag)*num_flags);

					/*
						Gets all flags' position
					*/
					flags_index =  malloc(sizeof(int)*num_flags);
					for(i = 0; i < num_flags; i++){
						msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
						flags_index[i] = atoi(msg_queue.mtext);
					}

					save_flags();

					/*
						Foreach pawn gets the nearest flag and tells the pawn
						where to go
					*/
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

					/*
						Tells the master he has given all instructions
					*/
					msg_queue.mtype = (long) getppid();
					msgsnd(msg_id, &msg_queue, LENGTH, 0);
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
					tells the master that a flag has been caught and sends points
				 */
				case CAUGHT:
					split_msg = strtok (NULL, " ");
					points =  atoi(split_msg);
					split_msg = strtok (NULL, " ");
					caught_index =  atoi(split_msg);
					msg_queue.mtype = (long) getppid();
					sprintf(msg_queue.mtext, "%d %d %d %d", CAUGHT, getpid(), points, caught_index);
					msgsnd(msg_id, &msg_queue, LENGTH, 0);
					break;

				/*
					when a flag is caught, check if directions need to be changed
				 */
				case ALRM:
					handle_alarm();
					break;

				/*
					when the round ended gets current moves left, sends to
					the master his total moves left and frees allocations
				 */
				case END_ROUND:

					/*
						sends END_ROUND and gets moves left
					*/
					for(i = 0; i < SO_NUM_P; i++){
						msg_queue.mtype = (long)(self_pawns[i].pid);
						sprintf(msg_queue.mtext, "%d", END_ROUND);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}

					for(i = 0; i < SO_NUM_P; i++){
						msgrcv(prt_msg_id, &prt_msg_queue, LENGTH, rcv, 0);
						current_moves += atoi(prt_msg_queue.mtext);
					}

					/*
						sends moves lefts
					*/
					prt_msg_queue.mtype = (long) getppid();
					sprintf(prt_msg_queue.mtext, "%d %d", getpid(), current_moves);
					msgsnd(prt_msg_id, &prt_msg_queue, LENGTH, 0);

					/*
						frees current flags and flags_index allocations
					*/
					free(flags);
					flags = NULL;
					free(flags_index);
					flags_index = NULL;
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
 * saves all flags and marks all of them as available 'Y'
 */
void save_flags(){
	int i;

	for(i = 0; i < num_flags; i++){
		flags[i].x = board[flags_index[i]].x;
		flags[i].y = board[flags_index[i]].y;
		flags[i].available = 'Y';
	}
}

/**
 * updates the flag's list, if a flag has been caught
 */
void update_flags(){
	int i;

	for(i = 0; i < num_flags; i++){
		if(board[flags_index[i]].type != 'f'){
			flags[i].available = 'N';
		}
	}
}

/**
 * gets the flag with minimum distance
 * @param  pawn pawn from which to calculate the distance
 * @return      coordinates (x,y) of the flag
 */
struct Pair min_dist_flag(Pawn pawn){
	int i, index = -1, dist, min = SO_BASE*SO_ALTEZZA + 1;
	struct Pair p, current_flag;

	p.x = -1;
	p.y = -1;

	/*
		Foreach available flag, calculates the distance from a pawn and
		stores the minimum
	*/
	for(i = 0; i < num_flags; i++){
		if(flags[i].available == 'Y'){
			current_flag.x = flags[i].x;
			current_flag.y = flags[i].y;
			dist = abs(current_flag.x - pawn.x) + abs(current_flag.y - pawn.y);
			if(dist < min) {
				min = dist;
				index = i;
			}
		}
	}

	if (index > -1){
		p.x = flags[index].x;
		p.y = flags[index].y;
	}
	return p;
}

/**
 * for all pawns sends MOVE_TO msg
 * and coordinate (x,y) of the new target flag to be caught
 */
void handle_alarm(){
	int i, target_ind, pawn_x, pawn_y;
	long rcv;
	char *split_msg;

	struct Pair target;
	struct msgbuf msg_queue;
	struct msgbuf prt_msg_queue;

	rcv = (long) getpid();

	update_flags();

	/*
		checks pawn to redirect
	*/
	for(i = 0; i < SO_NUM_P; i++){
		target_ind = targets[i].y * SO_BASE + targets[i].x;

		if(board[target_ind].type != 'f'){

			msg_queue.mtype = (long)(self_pawns[i].pid);
			sprintf(msg_queue.mtext, "%d", POSITION);
			msgsnd(msg_id, &msg_queue, LENGTH, 0);

			/*
				Gets current position from a pawn
			*/
			msgrcv(prt_msg_id, &prt_msg_queue, LENGTH, rcv, 0);
			split_msg = strtok (prt_msg_queue.mtext," ");
			pawn_x = atoi(split_msg);
			split_msg = strtok (NULL, " ");
			pawn_y = atoi(split_msg);

			self_pawns[i].x = pawn_x;
			self_pawns[i].y = pawn_y;

			/*
				calculate the new target, if exists sends it to the pawn
				with the START_MOVING msg
			*/
			target = min_dist_flag(self_pawns[i]);
			if(target.x > -1 && target.y > -1){
				targets[i].x = target.x;
				targets[i].y = target.y;
				msg_queue.mtype = (long)(self_pawns[i].pid);
				sprintf(msg_queue.mtext, "%d", MOVE_TO);
				msgsnd(msg_id, &msg_queue, LENGTH, 0);
				msg_queue.mtype = (long)(self_pawns[i].pid);
				sprintf(msg_queue.mtext, "%d %d", target.x, target.y);
				msgsnd(msg_id, &msg_queue, LENGTH, 0);
				msg_queue.mtype = (long)(self_pawns[i].pid);
				sprintf(msg_queue.mtext, "%d", START_MOVING);
				msgsnd(msg_id, &msg_queue, LENGTH, 0);
			}
		}
	}
}

/**
 * handles SIGTERM signal, terminates all pawns, frees all structs
 * and exits with left moves
 * @param signal SIGTERM
 */
void handle_term(int signal){
	int i, status, left_moves = 0;
    pid_t child_pid;

	/*
		Sends SIGTERM to self pawns
	*/
    for(i=0; i<SO_NUM_P; i++){
		kill(self_pawns[i].pid, SIGTERM);
	}

    i = 0;
    while ((child_pid = waitpid(self_pawns[i].pid,&status,0)) != -1) {
		if (WIFEXITED(status)) {
			/*
				gets pawn left moves from exit status
			*/
			left_moves += WEXITSTATUS(status);
			i++;
 		}else{
            printf("[PLAYER %d] pawn exit by signal %d\n", getpid(), 128 + WTERMSIG(status));
        }
	}
	if (errno != ECHILD) {
		fprintf(stderr, "[PLAYER_ERROR] PLAYER %d error #%d: %s\n", getpid(), errno, strerror(errno));
		free(self_pawns);
		free(targets);
		if(flags != NULL) free(flags);
		if(flags_index != NULL) free(flags_index);
		exit(EXIT_FAILURE);
	}

	/*
		Frees allocations
	*/
    free(self_pawns);
	free(targets);
	if(flags != NULL) free(flags);
	if(flags_index != NULL) free(flags_index);

    printf("[PLAYER %d] Bye...\n", getpid());
    _exit(left_moves);
}
