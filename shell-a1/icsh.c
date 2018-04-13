/* Keep track of attributes of the shell.  */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <setjmp.h>
#include <termios.h>

/* A process is a single process.  */
typedef struct process
{
  struct process *next;       /* next process in pipeline */
  char **argv;                /* for exec */
  pid_t pid;                  /* process ID */
  char completed;             /* true if process has completed */
  char stopped;               /* true if process has stopped */
  int status;                 /* reported status value */
} process;

/* A job is a pipeline of processes.  */
typedef struct job
{
  struct job *next;           /* next active job */
  char *command;              /* command line, used for messages */
  process *first_process;     /* list of processes in this job */
  pid_t pgid;                 /* process group ID */
  char notified;              /* true if user told about stopped job */
  struct termios tmodes;      /* saved terminal modes */
  int stdin, stdout, stderr;  /* standard i/o channels */
  int foreground;
  int background;
} job;

/* The active jobs are linked into a list.  This is its head.   */
job *first_job = NULL;

pid_t shell_pgid;
struct termios shell_tmodes;
int shell_terminal;
int shell_is_interactive;
int exit_status;
int background;
int fore_id = -1;

static sigjmp_buf env;
static volatile sig_atomic_t jump_active = 0;

/* Find the active job with the indicated pgid.  */
job *
find_job (pid_t pgid)
{
  job *j;

  for (j = first_job; j; j = j->next)
    if (j->pgid == pgid)
      return j;
  return NULL;
}

/* Return true if all processes in the job have stopped or completed.  */
int
job_is_stopped (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    if (!p->completed && !p->stopped)
      return 0;
  return 1;
}

/* Return true if all processes in the job have completed.  */
int
job_is_completed (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    if (!p->completed)
      return 0;
  return 1;
}

void launch_process (process *p, pid_t pgid, int infile, int outfile, int errfile, int foreground){
  pid_t pid;

  if (shell_is_interactive)
    {
      /* Put the process into the process group and give the process group
         the terminal, if appropriate.
         This has to be done both by the shell and in the individual
         child processes because of potential race conditions.  */
      pid = getpid ();
      if (pgid == 0) pgid = pid;
      setpgid (pid, pgid);
      if (foreground)
        tcsetpgrp (shell_terminal, pgid);

      /* Set the handling for job control signals back to the default.  */
      signal (SIGINT, SIG_DFL);
      signal (SIGQUIT, SIG_DFL);
      signal (SIGTSTP, SIG_DFL);
      signal (SIGTTIN, SIG_DFL);
      signal (SIGTTOU, SIG_DFL);
      signal (SIGCHLD, SIG_DFL);
    }

  /* Set the standard input/output channels of the new process.  */
  if (infile != STDIN_FILENO)
    {
      dup2 (infile, STDIN_FILENO);
      close (infile);
    }
  if (outfile != STDOUT_FILENO)
    {
      dup2 (outfile, STDOUT_FILENO);
      close (outfile);
    }
  if (errfile != STDERR_FILENO)
    {
      dup2 (errfile, STDERR_FILENO);
      close (errfile);
    }

  /* Exec the new process.  Make sure we exit.  */
  execvp (p->argv[0], p->argv);
  perror ("execvp");
  exit (1);
}


/* Make sure the shell is running interactively as the foreground job
   before proceeding. */

void init_shell ()
{

  /* See if we are running interactively.  */
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty (shell_terminal);

  if (shell_is_interactive)
    {
      /* Loop until we are in the foreground.  */
      while (tcgetpgrp (shell_terminal) != (shell_pgid = getpgrp ()))
        kill (- shell_pgid, SIGTTIN);

      /* Ignore interactive and job-control signals.  */
      signal (SIGINT, SIG_IGN);
      signal (SIGQUIT, SIG_IGN);
      signal (SIGTSTP, SIG_IGN);
      signal (SIGTTIN, SIG_IGN);
      signal (SIGTTOU, SIG_IGN);
      signal (SIGCHLD, SIG_IGN);

      /* Put ourselves in our own process group.  */
      shell_pgid = getpid ();
      if (setpgid (shell_pgid, shell_pgid) < 0)
        {
          perror ("Couldn't put the shell in its own process group");
          exit (1);
        }

      /* Grab control of the terminal.  */
      tcsetpgrp (shell_terminal, shell_pgid);

      /* Save default terminal attributes for shell.  */
      tcgetattr (shell_terminal, &shell_tmodes);
    }
}
/* Check for processes that have status information available,
   blocking until all processes in the given job have reported.  */

void wait_for_job (job *j){
  int status;
  pid_t pid;

  do
    pid = waitpid(WAIT_ANY, &status, WUNTRACED);

  while (!mark_process_status (pid, status)
         && !job_is_stopped (j)
         && !job_is_completed (j));
  exit_status =  WEXITSTATUS(status);
}

/* Store the status of the process pid that was returned by waitpid.
   Return 0 if all went well, nonzero otherwise.  */

int mark_process_status (pid_t pid, int status) {
  job *j;
  process *p;

  if (pid > 0)
    {
      /* Update the record for the process.  */
      for (j = first_job; j; j = j->next)
        for (p = j->first_process; p; p = p->next)
          if (p->pid == pid)
            {
              p->status = status;
              if (WIFSTOPPED (status))
                p->stopped = 1;
              else
                {
                  p->completed = 1;
                  if (WIFSIGNALED (status))
                    fprintf (stderr, "%d: Terminated by signal %d.\n",
                             (int) pid, WTERMSIG (p->status));
                }
              return 0;
             }
      fprintf (stderr, "No child process %d.\n", pid);
      return -1;
    }
  // else if (pid == 0 || errno == ECHILD)
    /* No processes ready to report.  */
    // return -1;
  else {
    /* Other weird errors.  */
    perror ("waitpid");
    return -1;
  }
}

/* Check for processes that have status information available,
   without blocking.  */

void update_status (void)
{
  int status;
  pid_t pid;

  do
    pid = waitpid (WAIT_ANY, &status, WUNTRACED|WNOHANG);
  while (!mark_process_status (pid, status));
}


/* Format information about job status for the user to look at.  */

void format_job_info(job *j, const char *status) {
  fprintf (stderr, "%ld (%s): %s\n", (long)j->pgid, status, j->command);
}

/* Put job j in the foreground.  If cont is nonzero,
   restore the saved terminal modes and send the process group a
   SIGCONT signal to wake it up before we block.  */

void put_job_in_foreground (job *j, int cont){
  /* Put the job into the foreground.  */

  //set fg to 1 & bg to 0
  j->foreground = 1;
  j->background = 0;

  tcsetpgrp (shell_terminal, j->pgid);

  /* Send the job a continue signal, if necessary.  */
  if (cont)
    {
      tcsetattr (shell_terminal, TCSADRAIN, &j->tmodes);
      if (kill (- j->pgid, SIGCONT) < 0){
        perror ("kill (SIGCONT)");
      }
      else {
        format_job_info(j, "foreground");
      }
    }

  /* Wait for it to report.  */
  wait_for_job (j);

  /* Put the shell back in the foreground.  */
  tcsetpgrp (shell_terminal, shell_pgid);

  /* Restore the shell’s terminal modes.  */
  tcgetattr (shell_terminal, &j->tmodes);
  tcsetattr (shell_terminal, TCSADRAIN, &shell_tmodes);
}

/* Put a job in the background.  If the cont argument is true, send
   the process group a SIGCONT signal to wake it up.  */

void put_job_in_background (job *j, int cont){
  //set fg to 0 & bg to 1
  j->foreground = 0;
  j->background = 1;
  /* Send the job a continue signal, if necessary.  */
  if (cont)
    if (kill (-j->pgid, SIGCONT) < 0) {
      perror ("kill (SIGCONT)");
    }
    else {
      format_job_info(j, "background");
    }
}

process *make_proc() {
  process *p = (process *)malloc(sizeof(process));
  p->next = NULL;
  p->argv = NULL;
  p->pid = 0;
  p->completed = 0;
  p->stopped = 0;
  p->status = 0;
  return p;
}

void launch_job (job *j, int foreground) {
  process *p = make_proc();
  pid_t pid;
  int mypipe[2], infile, outfile;

  infile = j->stdin;
  for (p = j->first_process; p; p = p->next)
    {
      /* Set up pipes, if necessary.  */
      if (p->next)
        {
          if (pipe (mypipe) < 0)
            {
              perror ("pipe");
              exit (1);
            }
          outfile = mypipe[1];
        }
      else
        outfile = j->stdout;

      /* Fork the child processes.  */
      pid = fork ();
      if (pid == 0)
        /* This is the child process.  */
        launch_process (p, j->pgid, infile,
                        outfile, j->stderr, foreground);
      else if (pid < 0)
        {
          /* The fork failed.  */
          perror ("fork");
          exit (1);
        }
      else
        {
          /* This is the parent process.  */
          p->pid = pid;
          if (shell_is_interactive)
            {
              if (!j->pgid)
                j->pgid = pid;
              setpgid (pid, j->pgid);
            }
        }

      /* Clean up after pipes.  */
      if (infile != j->stdin)
        close (infile);
      if (outfile != j->stdout)
        close (outfile);
      infile = mypipe[0];
    }

  format_job_info (j, "launched");

  if (!shell_is_interactive)
    wait_for_job (j);
  else if (foreground)
    put_job_in_foreground (j, 0);
  else
    put_job_in_background (j, 0);
}

job *make_job() {
  job *j = (job *)malloc(sizeof(job));
  j->command = 0;
  j->first_process = NULL;
  j->pgid = 0;
  j->notified = 0;
  j->stdin = STDIN_FILENO;
  j->stdout = STDOUT_FILENO;
  j->stderr = STDERR_FILENO;
  j->foreground = 1;
  j->background = 0;
  j->next = NULL;
  return j;
}

char **get_input(char *input) {
    char **command = malloc(8 * sizeof(char *));
    if (command == NULL) {
        perror("malloc failed");
        exit(1);
    }

    char *separator = " ";
    char *parsed;
    int index = 0;

    parsed = strtok(input, separator);
    while (parsed != NULL) {
        if(strcmp(parsed, "&") == 0) {
          background = 1;
        }
        command[index] = parsed;
        index++;
        parsed = strtok(NULL, separator);
    }

    command[index] = NULL;
    return command;
}

int icsh_cd(char **args) {
  if (args[1] == NULL) {
    fprintf(stderr, "icsh: expected argument to \"cd\"\n");
  }
  else if (chdir(args[1]) < 0) {
      perror(args[1]);
  }
}

int icsh_echo_status(process *p) {
  if(p->argv[1] == NULL){
    fprintf(stderr, "icsh: expected $?\n");
  }
  else if (strcmp(p->argv[1], "$?") == 0) {
    printf("Exit status: %d\n", exit_status);
  }
  return 0;
}

void sigint_handler(int signo) {
    if (!jump_active) {
        return;
    }
    siglongjmp(env, 42);
}

int main() {
    init_shell();
    job *j = make_job();

    char **command;
    char *input;
    pid_t child_pid;
    int stat_loc;

    int counter = 1;

    /* Setup SIGINT */
    struct sigaction s;
    s.sa_handler = sigint_handler;
    sigemptyset(&s.sa_mask);
    s.sa_flags = SA_RESTART;
    sigaction(SIGINT, &s, NULL);


    while (1) {
        if (sigsetjmp(env, 1) == 42) {
            printf("\n");
            continue;
        }
        background = 0;
        jump_active = 1;

        input = readline("icsh> ");
        if (input == NULL) {  /* Exit on Ctrl-D */
            printf("\n");
            exit(0);
        }
        command = get_input(input);

        if (!command[0]) {      /* Handle empty commands */
            free(input);
            free(command);
            continue;
        }

        else if (strcmp(command[0], "cd") == 0 && !background) {
            icsh_cd(command);
            /* Skip the fork, so that it doesnt work on anothe process */
            continue;
            update_status();
        }

        else if(strcmp(command[0], "Exit") == 0) {
          update_status();
          exit(0);
        }
        else if(strcmp(command[0], "echo") == 0 && strcmp(command[1], "$?") == 0 && !background){
          update_status();
          printf("Exit status: %d\n", exit_status);
          command = NULL;
        }
        else if(strcmp(command[0], "fg") == 0){
          launch_job(j,j->foreground);
          put_job_in_foreground(j,0);
        }
        else if(strcmp(command[0], "bg") == 0){
          launch_job(j,j->background);
          put_job_in_background(j,0);
        }

        child_pid = fork();
        if (child_pid < 0) {
            perror("Fork failed");
            exit(1);
        }

        if (child_pid == 0) {
            struct sigaction s_child;
            s_child.sa_handler = sigint_handler;
            sigemptyset(&s_child.sa_mask);
            s_child.sa_flags = SA_RESTART;
            sigaction(SIGINT, &s_child, NULL);

            /* Never returns if the call is successful */
            if (execvp(command[0], command) < 0) {
                perror(command[0]);
                exit(1);
            }
        } else {
            if(background == 0) {
              waitpid(child_pid, &stat_loc, WUNTRACED);
              // exit_status = WEXITSTATUS(stat_loc);
            } else {
              printf("[%d] %d\n", counter, child_pid);
              waitpid(child_pid, &stat_loc, WUNTRACED);
              counter++;

            }
        }

        if (!input)
            free(input);
        if (!command)
            free(command);
    }

    return 0;
}
