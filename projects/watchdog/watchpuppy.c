#include <stdlib.h>/* EXIT_FAILURE */
#include <stdio.h> /* printf */
#include <sys/sem.h> /*sembuf*/
#include <signal.h> /* sigaction */
#include <unistd.h> /* sleep */
#include <pthread.h>/*threads*/
#include <assert.h>/*assert*/
#include <string.h>/*memset*/
#include <errno.h>/*errno*/
#include <string.h>/*memset*/

#include "watchdog.h"
#include "scheduler.h"

#define WD_PATH "./wd"
#define UNUSED(X) (void)(X)
#define FREQUENCY 1

union semun
{
    int val;  
    struct semid_ds *buf;  
    unsigned short *array;  
    struct seminfo *__buf;  
};

pthread_t thread;
int g_wd_pid = -1;
int g_sem_id = 0;
char **new_argv = NULL;
volatile sig_atomic_t g_got_signal = 0;
volatile sig_atomic_t g_got_usr2 = 0;
int g_should_stop = 0;

static int GetSemVal(int semnum);
static char **MakeArgv(char *argv[], int argc, int proj_id, int *status);
static long HeartBeat(void *params);
static long CheckWatchDog(void *params);
static void *SchedulerRun(void *scheduler);
static int RunThread(scd_t *scheduler);
static int SemInit(int proj_id, char **argv);
static void Sigusr1_handler(int sig, siginfo_t *info, void *context);
static void Sigusr2_handler(int signum);
static void SignalsInit();
static int ReviveWD(char *argv[]);
static int SemDestroy();
static void DestroyArgv(char *argv[]);
static int CreateWatchDogIfNeeded();

int WDStart(int argc, char *argv[], int proj_id)
{
	key_t sem_key = 0;
    struct sembuf wd_ready = {0,-1,0};
    struct sembuf app_ready = {1,1,0};
	scd_t *scheduler = ScdCreate();
	int status = 0;

	assert(argv);
	assert(argc > 0);

	SignalsInit();
	if(SemInit(proj_id, argv) == WD_E_SEM)
	{
		return WD_E_SEM;
	}

	new_argv = MakeArgv(argv, argc, proj_id, &status);

    ScdAdd(scheduler, FREQUENCY, HeartBeat, NULL);
	ScdAdd(scheduler, FREQUENCY, CheckWatchDog, new_argv);

    if (semop(g_sem_id, &app_ready, 1) == -1) 
    {
		return WD_E_SEM;
    }	

	status = CreateWatchDogIfNeeded();
	if(status)
	{
		return status;
	}

    status = RunThread(scheduler);
    if(status)
    {
    	return status;
    }

    return status;
}

void WDStop()
{
	g_should_stop = 1;
	int times = 5;

	while(times && !g_got_usr2)
	{
		kill(g_wd_pid, SIGUSR2);
		sleep(1);
		--times;

    }
    kill(g_wd_pid, SIGKILL);

    DestroyArgv(new_argv);
    SemDestroy(g_sem_id);

	pthread_join(thread, NULL);
}

static int CreateWatchDogIfNeeded()
{
	struct sembuf wd_ready = {0,-1,0};

	if(0 == GetSemVal(0))
    {
    	g_wd_pid = fork();
    	if (g_wd_pid < 0) 
    	{ 
    		return WD_E_EXEC;
    	}
    	if(g_wd_pid == 0)
    	{
    		if(-1 == execv(new_argv[0], new_argv))
    		{
    			return WD_E_EXEC;
    		}
    		exit(0);
    	}

    	if (semop(g_sem_id, &wd_ready, 1) == -1) 
    	{
			return WD_E_SEM;
    	}
    }

    return 0;
}

static long HeartBeat(void *params)
{	
	UNUSED(params);
	
	if(!g_should_stop && g_wd_pid != -1)
	{
		kill(g_wd_pid, SIGUSR1);
	}
	else if(g_should_stop)
	{
		return -1;
	}

	return 0;			
}

static long CheckWatchDog(void *params)
{	
	UNUSED(params);

	static int lives = 3;
	if(g_should_stop)
	{
		return -1;
	}
	else
	{
		if(lives == 0 && !g_got_signal)
		{
			ReviveWD((char **)params);
			lives = 3;
		}
		else if(g_got_signal)
		{
			lives = 3;
			g_got_signal = 0;
		}
		else
		{
			--lives;
		}
	}
	return 0;			
}

static int ReviveWD(char *argv[])
{
	struct sembuf wd_ready = {0,-1,0};
	struct sembuf app_ready = {1,1,0};

    assert(argv);

	if (semop(g_sem_id, &wd_ready, 1) == -1) 
    {
		return WD_E_SEM;
    }

	g_wd_pid = fork();
	if (g_wd_pid < 0) 
    { 
    	return WD_E_EXEC;
    }
	else if(g_wd_pid == 0)
	{
		execv(argv[0], argv);
		exit(0);
	}

    if (semop(g_sem_id, &wd_ready, 1) == -1) 
    {
		return WD_E_SEM;
    }

    return 0;
}

static int RunThread(scd_t *scheduler)
{
	assert(scheduler);

    if (0 != pthread_create(&thread, NULL, SchedulerRun, scheduler))
    {
		return WD_E_THREAD;
    }

	return 0;
}

static void *SchedulerRun(void *scheduler)
{
	assert(scheduler);

	ScdRun((scd_t *)scheduler);
	ScdDestroy(scheduler);

	return NULL;
}

static int GetSemVal(int semnum)
{
	union semun arg;  

    return semctl(g_sem_id, semnum, GETVAL, arg);
}

static int SemDestroy()
{
	if(-1 == semctl(g_sem_id, 0, IPC_RMID))
	{
		return WD_E_SEM;
	}

	return 0;
}

static char **MakeArgv(char *argv[], int argc, int proj_id, int *status)
{
	int i = 0;
	char **buffer = NULL;
	char *num_buffer = NULL;

	assert(argv);

	num_buffer = (char *)malloc(sizeof(*num_buffer) * 8);
	if(NULL == num_buffer)
	{
		*status = WD_E_MEM;
	}

	buffer = (char **)malloc((argc + 3) * sizeof(*buffer));
	if(NULL == buffer)
	{
		*status = WD_E_MEM;
	}

	sprintf(num_buffer, "%d", proj_id);
	buffer[0] = WD_PATH;
	buffer[1] = num_buffer;
	
	i = 2;
	while(argv[i - 2] != NULL)
	{
		buffer[i] = argv[i - 2];
		++i;
	}
	buffer[i] = NULL;

	return buffer;
}

static void DestroyArgv(char *argv[])
{
	assert(argv);

	free(argv[1]);
	free(argv);
}

static int SemInit(int proj_id, char **argv)
{
	int sem_key = 0;

	assert(argv);

	if ((sem_key = ftok(argv[0], proj_id)) == (key_t) -1) 
    {
    	return WD_E_SEM;
    }

    g_sem_id = semget(sem_key, 2, IPC_CREAT | IPC_EXCL | 0600);
    if (errno == EEXIST)
    {
    	g_sem_id = semget(sem_key, 2, 0600);
    }
}

static void SignalsInit()
{
	struct sigaction action1;
	struct sigaction action2;

    memset(&action1,0,sizeof(struct sigaction));
    memset(&action2,0,sizeof(struct sigaction));

	action1.sa_flags = SA_SIGINFO;
    action1.sa_sigaction = Sigusr1_handler;
    sigaction(SIGUSR1, &action1, NULL);

    action2.sa_handler = Sigusr2_handler;
    sigaction(SIGUSR2, &action2, NULL);
}

static void Sigusr1_handler(int sig, siginfo_t *info, void *context)
{
	printf("I am the watch |puppy| and i got the signal\n");
	if(g_wd_pid == -1)
	{
		g_wd_pid = info->si_pid;
	}
	else
	{
		g_got_signal = 1;
	}
}

static void Sigusr2_handler(int signum)
{
	UNUSED(signum);
	g_got_usr2 = 1;
}