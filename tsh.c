/*
 * tsh - A tiny shell program with job control
 * 
 * @author Somsubhra Bairi (201101056@daiict.ac.in)
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
pid_t Fork(void);
int Sigprocmask(int action, sigset_t* set, void*);
int Sigaddset(sigset_t *set, int signal);
int Sigemptyset(sigset_t* set);
int Setpgid(int a, int b);
int Kill(pid_t pid, int signal);
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
    char *argv[MAXARGS];                                                        //arguments for execve()
    int bg;                                                                     //Determines whether the job will run in foreground or background
    pid_t pid;                                                                  //Contains the process id
    struct job_t *jd;
    sigset_t mask;                                                              //The signal set which has to be bloacked before adding the job to jobs

    bg = parseline(cmdline, argv);                                              //Copies contents of cmdline into argv and returns whether the job should run in background or foreground

    Sigemptyset(&mask);                                                         //Generate an empty signal set in mask
    Sigaddset(&mask, SIGCHLD);                                                  //Add SIGCHLD to the signal set to be blocked
    Sigaddset(&mask, SIGINT);                                                   //Add SIGINT to the signal set to be blocked
    Sigaddset(&mask, SIGTSTP);                                                  //Add SIGTSTP to the signal set to be blocked

    if(!builtin_cmd(argv)){                                                     //Checks whether command is built-in and executes it if yes, else enters if block
        Sigprocmask(SIG_BLOCK, &mask, NULL);                                    //Blocked the signal set
        if((pid = Fork()) == 0){                                                //Run user process in a child
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);                              //Unblock the signal sets in child
            Setpgid(0,0);                                                       //New jobs should have new process ids else signal will kill shell also          
            if(execve(argv[0], argv, environ) < 0){                             //executes user command if successful
                printf("%s: Command not found.\n", argv[0]);                    //Throw error if execution unsuccessful
                exit(0);
            }
        }

        if(!bg){                                                                //If process is foreground, parent waits for the job to terminate
            addjob(jobs, pid, FG, cmdline);                                     //Add the process to jobs
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);                              //Unblock the signal set afet adding the job
            waitfg(pid);                                                        //Parent waits for the foreground process to terminate}
        }

        else{                                                                   //If process is a background
            addjob(jobs, pid, BG, cmdline);                                     //Add the process to jobs
            Sigprocmask(SIG_UNBLOCK, &mask, NULL);                              //Unblock the signal set afet adding the job
            jd = getjobpid(jobs, pid);                                          //Get the jobpid
            printf("[%d] (%d) %s", jd->jid, jd->pid, jd->cmdline);              //Print the details of background job
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
    if(!strcmp(argv[0], "quit")){                                                   //If argument is quit
        exit(0);                                                                    //exit the shell
    }

    if(!strcmp(argv[0], "jobs")){                                                   //If argument is jobs
        listjobs(jobs);                                                             //List all the jobs
        return 1;
    }

    if(!strcmp(argv[0], "bg")){                                                     //If argument is bg
        do_bgfg(argv);                                                              //jump to do_bgfg
        return 1;
    }

    if(!strcmp(argv[0], "fg")){                                                     //If argument is fg
        do_bgfg(argv);                                                              //jump to do_bgfg
        return 1;
    }

    return 0;                                                                       //not a builtin command
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    struct job_t* jd = NULL;                                                        //Store the job details

    if( argv[1] == NULL ){                                                          //If no second argument
        printf("%s command requires PID or %%jobid argument\n", argv[0]);           //throw error
        return;
    }

    if(argv[1][0] == '%'){                                                          //If % then job id

        if(!isdigit(argv[1][1])){                                                   //If not a digit
            printf("%s: argument must be a pid or %%jobid\n", argv[0]);             //throw error
            return;
        }

        int jid = atoi( &argv[1][1] );                                              //Get the jid
        if( !( jd = getjobjid( jobs, jid ) ) ){                                     //If no such job with that jid
            printf( "%s: no such job\n", argv[1] );                                 //throw error
            return;
        }
    }

    else{                                                                           //If no % then pid

        if(!isdigit(argv[1][0])){                                                   //If not a digit
            printf("%s: argument must be a pid or %%jobid\n", argv[0]);             //throw error
            return;
         }

        pid_t pid = atoi( argv[1] );                                                //get the pid
        if( !( jd = getjobpid( jobs, pid ) ) ){                                     //if no such job with that pid
            printf( "(%s): no such process\n", argv[1]);                            //throw error
            return;
        }
    }

    Kill(-jd->pid, SIGCONT);                                                        //Send SIGCONT signal

    if( !strcmp( argv[0],"bg" ) ){                                                  //If background
        jd->state = BG;                                                             //Change job state to BG
        printf("[%d] (%d) %s",jd->jid,jd->pid,jd->cmdline);                         //print status
    }

    else {                                                                          //If foreground
        jd->state = FG;                                                             //Change job state to FG
        waitfg( jd->pid );                                                          //Wait for the job to finish
    }

    return;
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    struct job_t *fg_job = getjobpid(jobs, pid);                                    //Get the foreground job
    if(!fg_job){                                                                    //If no foreground job
        return;                                                                     //return without doing anything
    }

    while(fg_job->pid == pid && fg_job->state == FG){                               //If job is FG
        sleep(1);                                                                   //then sleep
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
    pid_t child_pid;                                                                //Stores the child pid
    int status;                                                                     //Status variable

    while((child_pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0){               //Get the child pid in the loop
        struct job_t *jd = getjobpid(jobs, child_pid);                              //Get job detail of the child
        if(!jd){                                                                    //If no job
            printf("((%d): No such child", child_pid);                              //Throw error
            return;
        }

        if(WIFSTOPPED(status)){                                                     //If stopped
            jd->state = ST;                                                         //Change state of job to stopped
            printf("Job [%d] (%d) stopped by signal 20\n",jd->jid, child_pid);      //print the message
        }

        else if(WIFSIGNALED(status)){                                               //If signalled
            deletejob(jobs, child_pid);                                             //Delete job from jobs list
            printf("Job [%d] (%d) terminated by signal 2\n", jd->jid, child_pid);
        }

        else if(WIFEXITED(status)){                                                 //If exited
            deletejob(jobs, child_pid);                                             //Delete from jobs list
        }

        else{                                                                       //If nothing
            unix_error("waitpid error");                                            //throw error
        }
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
    pid_t fpid;                                                                     //Stores the pid of the foreground job
    fpid = fgpid(jobs);                                                             //get the pid of the foreground job

    if(fpid > 0){                                                                   //If there is a running foreground job
        Kill(-fpid, SIGINT);                                                        //Send SIGINT signal to all the processes in the foreground job
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    pid_t fpid;                                                                     //Stores the pid of the foreground job
    fpid = fgpid(jobs);                                                             //get the pid of the foreground job

    if(fpid > 0){                                                                   //If there is a running foreground job
        Kill(-fpid, SIGTSTP);                                                       //Send SIGTSTP signal to all the processes in the foreground job
    }
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

/**
 * @brief Fork Wrapper function for fork
 * @return The pid of the child
 */
pid_t Fork(void){
    pid_t pid;                                                                              //The pid of the child process

    if((pid = fork()) < 0){                                                                 //If fork is unsuccessful
        unix_error("Fatal: Fork Error!");                                                   //Throw error
    }

    return pid;                                                                             //Return pid of the child process to parent or 0 to child
}

/**
 * @brief Sigemptyset Wrapper function for sigemptyset
 * @param set The signal set to be generated
 * @return 0 if success, -1 if error
 */
int Sigemptyset(sigset_t* set){
    int status;                                                                             //The status of the function

    if((status = sigemptyset(set))){                                                        //If sigemptyset fails
        unix_error("Fatal: Sigemptyset Error!");                                            //Throw error
    }
    return status;
}

/**
 * @brief Sigaddset Wrapper function for sigaddset
 * @param set The signal set to be added in
 * @param signal The  signal to be added
 * @return 0 if success, -1 if error
 */
int Sigaddset(sigset_t *set, int signal){
    int status;                                                                             //The status of the function

    if((status = sigaddset(set, signal))){                                                  //If sigaddset fails
        unix_error("Fatal: Sigaddset Error!");                                              //throw error
    }
    return status;
}

/**
 * @brief Sigprocmask Wrapper function for sigprocmask
 * @param action The action to be carried on the set
 * @param set The signal set on which the action is to be done
 * @param t NULL
 * @return  0 if success, -1 if error
 */
int Sigprocmask(int action, sigset_t* set, void* t){
    int status;                                                                             //The status if the function

    if((status = sigprocmask(action, set, NULL))){                                          //If sigprocmask fails
        unix_error("Fatal: Sigprocmask Error!");                                            //throw error
    }

    return status;
}

/**
 * @brief Setpgid Wrapper function for setpgid
 * @param a
 * @param b
 * @return Negative if error
 */
int Setpgid(int a, int b){
    int status;                                                                             //The status of the function

    if((status = setpgid(a,b) < 0)){                                                        //If setpgid fails
        unix_error("Fatal: Setpgid Error!");                                                //throw error
    }

    return status;
}

/**
 * @brief Kill Wrapper function for kill
 * @param pid The process id to whick the signal is sent
 * @param signal The signal to be sent
 * @return Negative if error
 */
int Kill(pid_t pid, int signal){
    int status;                                                                             //The status of function

    if((status = kill(pid, signal) < 0)){                                                   //If kill fails
        unix_error("Fatal: Kill Error!");                                                   //throw error
    }
    return status;
}
