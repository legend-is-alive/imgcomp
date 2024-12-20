//----------------------------------------------------------------------------------------
// Code to launch or re-launch image aquire command, which can be:
//    raspistill        For older raspierry pi OS, or if you prefer to use legacy camera
//    libcamera-still   For Raspierry pi os buster (11) or newer
//    libcamera-vid     For Raspierry pi os buster (11) or newer and frame rates higher than 1.
//
// monitors that raspistill or libcamera is still producing images, restarts it if it stops.
// Matthias Wandel 2023
//
// Imgcomp is licensed under GPL v2 (see README.txt)
//----------------------------------------------------------------------------------------
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>

#include "imgcomp.h"
#include "config.h"
#include "jhead.h"

static int camera_prog_pid = 0;

static char OutNameSeq = 'a';

int relaunch_timeout = 10;
int give_up_timeout = 20;

int kill(pid_t pid, int sig);
//-----------------------------------------------------------------------------------
// Parse command line and launch.
//-----------------------------------------------------------------------------------
static pid_t do_launch_program(char * cmd_string)
{
    char * Arguments[51];
    int narg;
    int a;
    int wasblank = 1;

    // Note: NOT handling quoted strings or anything for arguments with spaces in them.
    narg=0;
    for (a=0;cmd_string[a];a++){
        if (cmd_string[a] != ' '){
            if (wasblank){
                if (narg >= 50){
                    fprintf(stderr, "too many command line arguments\n");
                    exit(0);
                }
                Arguments[narg++] = &cmd_string[a];
            }
            wasblank = 0;
        }else{
            cmd_string[a] = '\0';
            wasblank = 1;
        }
    }
    Arguments[narg] = NULL;

    //printf("%d arguments\n",narg);
    //for (a=0;a<narg;a++){
    //    printf("'%s'\n",Arguments[a]);
    //}
    
    int pid = fork();
    if (pid == -1){
        // Failed to fork.
        fprintf(Log,"Failed to fork off child process\n");
        return -1;
    }

    if(pid == 0){
        // Child takes this branch.
        execvp(Arguments[0], Arguments);
        fprintf(Log,"Failed to execute: %s\n",Arguments[0]);
        perror("Reason");
        exit(errno);
        return -1;
    }
    return pid;
}

//-----------------------------------------------------------------------------------
// Launch or re-launch raspistill or libcamera-still or libcamera-vid
//-----------------------------------------------------------------------------------
int relaunch_camera_prog(void)
{
    // Kill raspistill or libcamera if it's already running.
    if (camera_prog_pid){
        kill(camera_prog_pid,SIGKILL);
        // If we launched the camera program (raspistill or libcamera), 
		// need to call wait() so that we dont' accumulate an army of child zombie processes
        int exit_code = 123;
        int a;
        time_t then, now = time(NULL);
        a = wait(&exit_code);
        fprintf(Log,"Child exit code %d, wait returned %d",exit_code, a);
        then = time(NULL);
        fprintf(Log," At %02d:%02d (%d s)\n",(int)(then%3600)/60, (int)(then%60), (int)(then-now));
    } else {
        // kill libcamera or raspistill if it was already launched when we started.
		// In that case, we don't have a PID for it.
		char KillCmd[100];
		for (int a=0;a<80;a++){
			// Search for first space to get capture command name.
			if (camera_prog_cmd[a] == ' '){ 
				snprintf(KillCmd, 99, "killall -9 %.*s",a,camera_prog_cmd);
				(void)system(KillCmd);
				break;
			}
		}
    }

    fprintf(Log,"Launching camera program\n");

    int DashOOption = (strstr(camera_prog_cmd, " -o ") != NULL);

    static char cmd_appended[300];
    strncpy(cmd_appended, camera_prog_cmd, 200);

    if (strncmp(cmd_appended, "raspistill", 10) == 0) { // check if the aquire cmd is actually a raspistill cmd
        if (ExposureManagementOn) { // Exposure managemnt by imgcomp
            strcat(cmd_appended, GetRaspistillExpParms());
            if (DashOOption){
                fprintf(stderr, "Must not specify -o option with -exm option\n");
                exit(-1);
            }
        }

        if (!DashOOption){
            // No output specified with raspistill command  Add the option,
            // with a different prefix each time so numbers don't overlap.
            int l = strlen(cmd_appended);
            sprintf(cmd_appended+l," -o %s/out%c%%05d.jpg",DoDirName, OutNameSeq++);
            if (OutNameSeq >= 'z') OutNameSeq = 'a';
            //fprintf(Log,"Run program: %s\n",cmd_appended);
        }
    }
    else {
        fprintf(stderr, "aquire_cmd was not raspistill, not setting output or exposure settings\n");
    }

    camera_prog_pid = do_launch_program(cmd_appended);
    return 0;
}

static int SinceLightChange = 0;
static int MotionAccumulate = 0;
//-----------------------------------------------------------------------------------
// Launch external commands on motion (for turning the lights on)
//-----------------------------------------------------------------------------------
void DoMotionRun(int SawMotion)
{
    int NowSec = (int) time(NULL);
    
    static int LastMotion = 0;
    static int LightOn;
    static pid_t child_pid = 0;
    char CmdCopy[200];

    //fprintf(Log, "DoMotionRun %d %d %d\n",SawMotion, SinceLightChange, NowSec-LastMotion);

    if (child_pid > 0){
        // Check if child has exited.
        pid_t r = waitpid(child_pid, NULL, WNOHANG);
        if (r == child_pid || r == -1){
            child_pid = 0;
            fprintf(Log,"Motionrun Child exited %d\n",r);
        }else{
            fprintf(Log,"Child still running  r=%d\n",r);
        }
    }

    // Ignore "motion" events for a few seconds after we hit the light switch.
    SinceLightChange += 1;
    if (SawMotion || NowSec-LastMotion <= 2){
        MotionAccumulate += 5;
        if (MotionAccumulate > 1000) MotionAccumulate = 1000;
    }else{
        MotionAccumulate -= 1;
        if (MotionAccumulate < 0) MotionAccumulate = 0;
    }
    //printf("Ma = %4d\n",MotionAccumulate);    
        
    if (SinceLightChange > 3 && SawMotion){
		//if (!LightOn && NowSec - LastMotion > 2) fprintf(Log, "delayed light on\n");
        if (!LightOn && NowSec - LastMotion <= 2){
			// Want two motion events close together to avoid false triggers
			// which can happen on lighting changes or camera artifacts.
            if (lighton_run[0]){
                if (child_pid <= 0){
                    fprintf(Log, "Turn light ON\n");
                    strncpy(CmdCopy, lighton_run, 200);
                    child_pid = do_launch_program(CmdCopy);
                    SinceLightChange = 0;
                    LightOn = 1;
                }else{
                    fprintf(Log,"Turn lights ON (wait child exit first)\n");
                }
            }else{
                LightOn = 1;
            }
        }
        LastMotion = NowSec;
    }else if (LightOn){
        // Compute how long the light should be left on.
        int mm = MotionAccumulate; if (mm > 500) mm = 500;
        mm -= 30; if (mm < 0) mm = 0;
        
        int timeout = lightoff_min + (lightoff_max-lightoff_min)*mm/500;
        //printf("Ma=%4d  timeout=%d (%d-%d)\n",MotionAccumulate, timeout, lightoff_min,lightoff_max);
        
        if ((NowSec-LastMotion) > timeout){
            if (lightoff_run[0]){
                if (child_pid <= 0){
                    fprintf(Log, "Turn light OFF (%d sec timeout)\n",timeout);
                    strncpy(CmdCopy, lightoff_run, 200);
                    child_pid = do_launch_program(CmdCopy);
                    SinceLightChange = 0;
                    LightOn = 0;
                }else{
                    fprintf(Log,"Turn light OFF (wait for child exit first)\n");
                }
            }else{
                LightOn = 0;
            }
        }
    }
}

//-----------------------------------------------------------------------------------
// Manage camera program (libcamera or raspistill) - may need restarting if it dies or brightness changed too much.
//-----------------------------------------------------------------------------------
static int MsSinceImage = 0;
static int MsSinceLaunch = 0;
static int InitialBrSum;
static int InitialNumBr;
static int NumTotalImages;

int manage_camera_prog(int NewImages)
{
    int timeout;
    time_t now = time(NULL);

    MsSinceImage += 1000;
    MsSinceLaunch += 1000;

    if (NewImages > 0){
        MsSinceImage = 0;
        NumTotalImages += NewImages;
    }else{
        if (MsSinceImage >= 3000){
            fprintf(Log,"No new images, %d (at %d:%d)\n",MsSinceImage, (int)(now%3600/60), (int)(now%60));
        }
    }

    if (camera_prog_pid == 0){
        // Camera program has not been launched.
        fprintf(Log,"Initial launch of camera program\n");
        goto force_restart;
    }

    timeout = relaunch_timeout * 1000;
    if (MsSinceImage > timeout){
        // Not getting any images for 5 seconds or vide ofiles for 10.
        // Probably something went wrong with raspistill or raspivid.
        if (MsSinceLaunch > timeout){
            if (give_up_timeout && MsSinceImage > give_up_timeout * 1000){
                if (NumTotalImages >= 5){
					// sometimes camera system just hangs.  This was rare with raspistill, 
					// but with libcamera-still it happens more often (about every two months)
                    fprintf(Log,"Relaunch camera program didn't fix.  Reboot!.  (%d sec since image)\n",MsSinceImage/1000);
                    // force rotation of log.
                    LogFileMaintain(1);
                    MsSinceImage = 0; // dummy for now.
                    printf("Reboot now\n");   // Requires setuid bit of reboot to be set as reboot
                    int r = system("reboot"); // normally requires root prviledges.
                                              // do "sudo chmod +s /usr/sbin/reboot" so normal process can run it.
                    fprintf(Log,"reboot returned %d (should not return -- please set the SUID bit of reboot)\n",r);
                    exit(0);
                }else{
                    // Less than 5 images.  Probably left over from last run.
                    fprintf(Log,"Camera program never worked! Give up. %d sec\n",MsSinceImage/1000);
                    LogFileMaintain(1);
                    // A reboot wouldn't fix this!
                    exit(0);
                }
            }else{
                fprintf(Log,"No images for %d sec.  Relaunch camera program\n",MsSinceImage/1000);
                goto force_restart;
            }
        }
    }
    return 0;

force_restart:
    relaunch_camera_prog();
    MsSinceLaunch = 0;
    InitialBrSum = InitialNumBr = 0;
    SinceLightChange = 0;
    return 1;
}
