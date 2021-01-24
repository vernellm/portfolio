/* 
 * tsh - A tiny shell program with job control
 * 
 * Vernell Mangum
 * vtmangum@uno.edu
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Redirect Constants (from textbook pg. 894) */
#define DEF_MODE S_IRUSR|S_IWUSR|S_IWGRP|S_IRGRP|S_IROTH|S_IWOTH
#define DEF_UMASK S_IWGRP|S_IWOTH

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void do_redirect(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
		}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* Ignoring these signals simplifies reading from stdin/stdout */
    Signal(SIGTTIN, SIG_IGN);          /* ignore SIGTTIN */
    Signal(SIGTTOU, SIG_IGN);          /* ignore SIGTTOU */


    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    int bg;
    pid_t pid;
    bg = parseline(cmdline, argv);
    sigset_t maskSet, prev_mask;

    /* Adding SIGCHLD, SIGINT, and SIGTST to sigset_t maskSet to crate mask
	 * that blocks these three signals.
     */
    sigaddset(&maskSet, SIGCHLD);
    sigaddset(&maskSet, SIGTSTP);
    sigaddset(&maskSet, SIGINT);


    /* Ignores empty input */
    if (argv[0] == NULL)
    	return;

    if (!builtin_cmd(argv)) {
    	/* Blocks SIGCHLD signal so that the signal is not sent before
    	 * before the parent reaches addjob().
    	 */
    	sigprocmask(SIG_BLOCK, &maskSet, &prev_mask);

    	/* Assings fork() value to pid */
    	pid = fork();

    	/* Checks for error in fork() call */
    	if (pid < 0) {
    		fprintf(stderr, "fork error: %s\n", strerror(errno));
    		exit(0);
    	}

	    if (pid == 0) { /* Makes child program run the input */

    		/* Call to setpgid that puts the child in a new process group
    		 * which ensures that my shell will be the only process in 
    		 * the foreground process group.
    		 */
    		setpgid(0, 0);

    		/* Does the redirection specified by the cmdline */
    		do_redirect(argv);

    		/* Unblocks SIGCHLD signal so that the child process
    		 * can recieve the SIGCHLD signal again
    		 */
    		sigprocmask(SIG_UNBLOCK, &maskSet, NULL);

	    	if (execve(argv[0], argv, environ) < 0) {
	    		printf("%s: Command not found\n", argv[0]);
	    		exit(0);
	    	}
	    }

	    if (pid != 0) {

		   	if (bg) { /* Deals with background processes */
		    	addjob(jobs, pid, BG, cmdline); // adds job from cmdline into the job list

		    	/* Unblocks SIGCHLD signal so that the parent process
	    		 * can recieve the SIGCHLD signal again (background).
	    		 */
	    		sigprocmask(SIG_UNBLOCK, &maskSet, NULL);

		    	/* prints the JID and PID of a job that is added to the jobs list */
		    	printf("[%d] (%d) %s", getjobpid(jobs, pid)->jid, getjobpid(jobs, pid)->pid, cmdline);
		    }
		    
		    if (!bg) { // Deals with foreground processes
		    	/* add job to the job list */
		    	addjob(jobs, pid, FG, cmdline);

		    	/* Unblocks the signal set so that the child process
	    		 * can recieve those signals again (foreground).
	    		 */
	    		sigprocmask(SIG_UNBLOCK, &maskSet, NULL);

		    	/* waits for the forground process to terminate before you enter new prompt */
		    	waitfg(pid);
		    }
		}
	}

    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (!strcmp(argv[0], "quit")) { // Compares the first string of input to the string 'quit'
    	exit(0); // Exits the program
    }

    if (!strcmp(argv[0], "jobs")) { // Compares the first string of input to the string 'jobs' was passed in
    	listjobs(jobs); // Lists the jobs through given struct and method
    	return 1;
    }

    if (!strcmp(argv[0], "fg")) { // Compares the first string of input to the string 'jobs' was passed in
    	do_bgfg(argv); // Passes whatever is typed in cmdline and does the operation for "fg" as defined in do_bgfg()
    	return 1;
    }

    if (!strcmp(argv[0], "bg")) { // Compares the first string of input to the string 'jobs' was passed in
    	do_bgfg(argv); // Passes whatever is typed in cmd and does the operation for "bg" as define in do_bgfg()
    	return 1;
    }

    return 0;     /* not a builtin command */
}

/* 
 * do_redirect - scans argv for any use of < or > which indicate input or output redirection
 *
 */
void do_redirect(char **argv)
{
    int i;

    for(i=0; argv[i]; i++)
    {
        if (!strcmp(argv[i],"<"))
        {
    		/* Opens the file for input as specified by the cmdline */
    		int fd = open(argv[i+1], O_RDONLY, 0);

    		/* Redirects the input to the new file/process as specified
    		 * by the command line 
    		 */
    		dup2(fd, 0);

    		/* Closes the file that was open for redirection */
    		close(fd);

            /* the line below cuts argv short -- this       
               removes the < and whatever follows from argv */
            argv[i]=NULL;
    	}
        else if (!strcmp(argv[i],">")) {

        	//umask(DEF_UMASK);
        		
    		/* Opens the file given by cmdline */
    		int fd = open(argv[i+1], O_CREAT|O_TRUNC|O_WRONLY, DEF_MODE);

    		/* redirects the output to the file specified by the cmdline */
    		dup2(fd, 1);

    		/* closes the file that was opened for redirection */
    		close(fd);

            /* the line below cuts argv short -- this       
               removes the > and whatever follows from argv */
            argv[i]=NULL;
        }
    }
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    /* If "fg" is passed in to the cmdline... */
    if (!strcmp(argv[0], "fg")) {

    	/* Checks to see if there are 0 arguments passed in after fg */
    	if (argv[1] == NULL) {
    		printf("fg command requires PID or %%jobid argument\n");
    		return;
	    }

    	/* Compares the second argument of the cmdline to see if "%" was passed in */
    	else if ((argv[1][0] == '%')) {
    		
    		/* Takes the string starting after the pointer and convert it to an int */
    		int jid = atoi(&argv[1][1]);

    		/* Checks to see if pid is in the jobs list */
    		if (jid > maxjid(jobs)) {
    			printf("%s: No such job\n", &argv[1][0]);
    			return;
    		}

    		/* Checks to see if the state is ST */
    		if ((getjobjid(jobs, jid)->state) == ST) {

	    		/* Updates the state to FG */
	    		getjobpid(jobs, getjobjid(jobs, jid)->pid)->state = FG;

	    		/* Sends the SIGCONT signal to to the job to resume it */
	    		kill(-getjobjid(jobs, jid)->pid, SIGCONT);

	    		/* Updates the state to FG */
	    		getjobpid(jobs, getjobjid(jobs, jid)->pid)->state = FG;
    		}

    		/* Checks to see if the state is BG */
    		if ((getjobjid(jobs, jid)->state) == BG) {

	    		/* Updates the state to FG */
	    		getjobjid(jobs, jid)->state = FG;
    		}

    		/* Calls waitfg() since it can only be ran by itself */
    		waitfg(getjobjid(jobs, jid)->pid);
    	}
    	else {

	    	/* Assigns the second argument of the cmdline to an int */
	    	int pid = atoi(argv[1]);

	    	/* Checks to see if the arguments passed in is an integer */
    		if (getjobpid(jobs, pid) != NULL) {

		    	/* Checks to see if the state is ST */
		    	if ((getjobpid(jobs, pid)->state) == ST) {

		    		/* Sends the SIGCONT signal to to the job to resume it */
		    		kill(-getjobpid(jobs, pid)->pid, SIGCONT);

		    		/* Updates the state to FG */
		    		getjobpid(jobs, pid)->state = FG;

		    		waitfg(pid);
		    	}

		    	/* Checks to see if the state is BG */
		    	if ((getjobpid(jobs, pid)->state) == BG) {

		    		/* Updates the state to FG */
		    		getjobpid(jobs, pid)->state = FG;

		    		waitfg(pid);
		    	}
		    }
		    else {
		    	/* Check to see if an integer was passed in, prints corresponding
		    	 * error message.
		    	 */
		    	if (pid > 0) {
					printf("(%d): No such process\n", pid);
    				return;
    			}
    			printf("fg: argument must be a PID or %%jobid\n");
		    }
	    }
    }

    /* If "bg" is passed in to the cmdline... */
    if (!strcmp(argv[0], "bg")) { // Compares the first argument to see if there is bg

    	/* Checks to see if there are 0 arguments passed in after fg */
    	if (argv[1] == NULL) {
    		printf("bg command requires PID or %%jobid argument\n");
    		return;
	    }

    	/* Compares the second argument of the cmdline to see if "%" was passed in */
    	if (argv[1][0] == '%') {

    		/* Assigns value after '%' to int jid */
    		int jid = atoi(&argv[1][1]);

    		/* Checks to see if pid is in the jobs list */
    		if (jid > maxjid(jobs)) {
    			printf("%s: No such job\n", &argv[1][0]);
    			return;
    		}

    		/* Sends the SIGCONT signal to the process, as specified from the cmdline. */
    		kill(-getjobjid(jobs, jid)->pid, SIGCONT);

    		/* Changes the state of the the process, as specified from the cmdline, to BG */
    		getjobjid(jobs, jid)->state = BG;

    		/* Print the JID, PID, and program name to the screen */
    		printf("[%d] (%d) %s", getjobjid(jobs, jid)->jid, getjobjid(jobs, jid)->pid, getjobjid(jobs, jid)->cmdline);
    	}
    	else {

	    	/* Assigns the second argment of the cmdline to an int */
	    	int pid = atoi(argv[1]);

	    	/* Checks to see if the arguments passed in is a character */
	    	if (pid == 0) {
	    		printf("bg: argument must be a PID or %%jobid\n");
	    		return;
	    	}

	    	/* If the pid passed in is not in the jobs list, print error message */
	    	if (getjobpid(jobs, pid) == NULL) {
	    		printf("(%d): No such process\n", pid);
	    		return;
	    	}

	    	/* Sends the SIGCONT signal to the the process to resume running. */
	    	kill(-pid, SIGCONT);

	    	/* Update the state of the process to BG */
	    	getjobpid(jobs, -pid)->state = BG;

	    	/* Print the JID, PID, and program name to the screen */
	    	printf("[%d] (%d) %s", getjobpid(jobs, pid)->jid, getjobpid(jobs, pid)->pid, getjobpid(jobs, pid)->cmdline);
    	}
    }

    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{

    while (fgpid(jobs) > 0) {
    	sleep(1);
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    int child_status;

    /* 
     * The following code was taken from forkSig.c and modified 
     * to work properly.
     */

	/* Reap each of the child processes */
	pid_t wpid = waitpid(-1, &child_status, WNOHANG|WUNTRACED);
		
	/* If the child exited without error, delete the job out of the jobs list. */
	if (WIFEXITED(child_status)) {
			
		/* Deletes the jobs out of the jobs list as they are terminated */
		deletejob(jobs, wpid);
	}

		/* If the child was signaled to stop by ctrl-c, read signal, print out message
		 * and delete job form the jobs list.
		 */
	if (WIFSIGNALED(child_status)) {
		
		/* prints out formatted description of the fg job that is being terminated */
		printf("Job [%d] (%d) terminated by signal %d\n", getjobpid(jobs, fgpid(jobs))->jid, fgpid(jobs), SIGINT);

		/* Deletes the jobs out of the jobs list as they are terminated */
		deletejob(jobs, wpid);
	}

	/* If the child was signaled to stop by ctrl-z, read in signal, change state, and print out message */
	if (WIFSTOPPED(child_status)) {
		
		/* prints out formatted description of the fg job that is being terminated */
		printf("Job [%d] (%d) stopped by signal %d\n", getjobpid(jobs, fgpid(jobs))->jid, fgpid(jobs), SIGTSTP);

		/* Sets the state of all foreground jobs to state 'stopped' */
		getjobpid(jobs, fgpid(jobs))->state = ST;
	}

    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	/* Cathes the SIGINT signal and sends it to the the foreground group */
	kill(-fgpid(jobs), SIGINT);

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{

    /* Catches the SIGSTP signal and sends it to the foreground group */
    kill(-fgpid(jobs), SIGTSTP);

    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



