 //-----------------------------------------------------------------------------------
// Simple tool for monitor continuously captured images for changes
// and saving changed images, as well as an image every N seconds for timelapses.
// Matthias Wandel 2015
//
// Imgcomp is licensed under GPL v2 (see README.txt)
//-----------------------------------------------------------------------------------
#include <stdio.h>
#include <ctype.h>		// to declare isupper(), tolower() 
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
    #include "readdir.h"
    #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
    #define strdup(a) _strdup(a) 
    extern void sleep(int);
    extern void usleep(int);    
    #define unlink(n) _unlink(n)
    #define PATH_MAX _MAX_PATH
#else
    #include <dirent.h>
    #include <unistd.h>
    #include <errno.h>
#endif

#include "imgcomp.h"
#include "config.h"

// Configuration variables.
char * progname;  // program name for error messages
char DoDirName[200];
char SaveDir[200];
char SaveNames[200];
char TempDirName[200];
int FollowDir = 0;
int ScaleDenom;
int SpuriousReject = 0;
int PostMotionKeep = 0;

int BrightnessChangeRestart = 1;
int SendTriggerSignals = 0;
int MsPerFrame;

char DiffMapFileName[200];
Regions_t Regions;

int Verbosity = 0;
char LogToFile[200];
char MoveLogNames[200];
FILE * Log;

int Sensitivity;
int Raspistill_restarted;
int TimelapseInterval;
char raspistill_cmd[200];
char blink_cmd[200];
char UdpDest[30];
//-----------------------------------------
// Tightening gap experiment hack
int GateDelay;
static int SinceMotionFrames = 1000;
extern int rzaveragebright; // Kind of a hack for mouse detection.  Detect mouse by brightness
                            // in the red (high sensitivity) region of the diffmap.

//-----------------------------------------
// Video mode hack
int VidMode; // Video mode flag
char VidDecomposeCmd[200];


typedef struct {
    MemImage_t *Image;
    char Name[500];
    int nind; // Name part index.
    unsigned  mtime;
    int DiffMag;
    int IsTimelapse;
    int IsMotion;
    int RzAverageBright;
}LastPic_t;

static LastPic_t LastPics[3];
static time_t NextTimelapsePix;
static LastPic_t NoMousePic;

time_t LastPic_mtime;


//-----------------------------------------------------------------------------------
// Convert picture coordinates from 120 degree fishey lens
// to pan and tilt angles for cap shooter.
//-----------------------------------------------------------------------------------
static void GeometryConvert(TriggerInfo_t  * Trig)
{
	double x,y, magxy, mag_rad;
	double px, py,ta;
	double pan,tilt;
	
	// Center-referenced coordinates.
	x = (Trig->x-1920/2)/1920.0;
	y = -(Trig->y-1440/2)/1920.0;
	magxy = sqrt(x*x+y*y); // Magnitude from center.
	
	// Convert to degrees from center and correct for lens distortion
	mag_rad = magxy * (109 + magxy*magxy * 21);
	mag_rad = mag_rad * 3.1415/180; // Convert to radians.
	
	// Now convert to planar coordinates.
	ta = tan(mag_rad);
	px = (ta*x)/magxy; // px and py are in "screen" meters from center
	py = (ta*y)/magxy;
	printf("px,py = %5.2f,%5.2f  ",px,py);
	
	// Now convert planar coordinates to pan angle and elevation, in degrees.
	pan = atan(px)*180/3.1415;
	tilt = atan(py/sqrt(1+px*px))*180/3.1415;
	
	printf("Pan: %5.1f tilt:%5.1f\n",pan,tilt);
	Trig->x = (int)(pan*10); // Return it as tenth's of a degree
	Trig->y = (int)(tilt*10);
}


//-----------------------------------------------------------------------------------
// Figure out which images should be saved.
//-----------------------------------------------------------------------------------
static int ProcessImage(LastPic_t * New, int DeleteProcessed)
{
    static int PixSinceDiff;

    LastPics[2] = LastPics[1];
    LastPics[1] = LastPics[0];

    LastPics[0] = *New;
    LastPics[0].IsMotion = LastPics[0].IsTimelapse = 0;

    if (LastPics[1].Image != NULL){
        TriggerInfo_t Trig;
        // Handle timelapsing.
        if (TimelapseInterval >= 1){
            if (LastPics[0].mtime >= NextTimelapsePix){
                LastPics[0].IsTimelapse = 1;
            }

            // Figure out when the next timelapse interval should be.
            NextTimelapsePix = LastPics[0].mtime+TimelapseInterval;
            NextTimelapsePix -= (NextTimelapsePix % TimelapseInterval);
        }

        // compare with previosu picture.
        Trig.DiffLevel = Trig.x = Trig.y = 0;

        if (LastPics[2].Image){
            Trig = ComparePix(LastPics[1].Image, LastPics[0].Image, 0, NULL);
        }
        LastPics[0].RzAverageBright = rzaveragebright;
        
       

        if (Trig.DiffLevel >= Sensitivity && PixSinceDiff > 5 && Raspistill_restarted){
            fprintf(Log,"Ignoring diff caused by raspistill restart\n");
            Trig.DiffLevel = 0;
        }
        LastPics[0].DiffMag = Trig.DiffLevel;

        if (FollowDir){
            // When real-time following, the timestamp is more useful than the file name
            char TimeString[10];
            strftime(TimeString, 10, "%H%M%S ", localtime(&LastPic_mtime));
            fprintf(Log,TimeString);
        }else{
            fprintf(Log,"%s: ",LastPics[0].Name+LastPics[0].nind);
        }
        if (Trig.DiffLevel){
            fprintf(Log,"%3d @(%4d,%4d) ", Trig.DiffLevel, Trig.x, Trig.y);
        }

        if (LastPics[0].DiffMag > Sensitivity){
            LastPics[0].IsMotion = 1;
        }

        if (SpuriousReject && LastPics[2].Image && 
            LastPics[0].IsMotion && LastPics[1].IsMotion
            && LastPics[2].DiffMag < (Sensitivity>>1)){
            // Compare to picture before last picture.
            Trig = ComparePix(LastPics[0].Image, LastPics[2].Image, 0, NULL);

            //printf("Diff with pix before last: %d\n",Trig.DiffLevel);
            if (Trig.DiffLevel < Sensitivity){
                // An event that was just one frame.  We assume this was something
                // spurious, like an insect or a rain drop
                printf("(spurious %d, ignore) ", Trig.DiffLevel);
                LastPics[0].IsMotion = 0;
                LastPics[1].IsMotion = 0;
            }
        }
        if (LastPics[0].IsMotion) fprintf(Log,"(motion) ");
        if (LastPics[0].IsTimelapse) fprintf(Log,"(time) ");

        if (LastPics[1].IsMotion) SinceMotionFrames = 0;

        if (SinceMotionFrames <= PostMotionKeep+1 || LastPics[2].IsTimelapse){
            // If it's motion, pre-motion, or timelapse, save it.
            if (SaveDir[0]){
                BackupImageFile(LastPics[2].Name, LastPics[2].DiffMag, 0);
            }
        }
        SinceMotionFrames += 1;
        
        //fprintf(Log," %d %d ",rzaveragebright, NoMousePic.RzAverageBright-20);
        
        fprintf(Log,"\n");
        
        
        if (Trig.DiffLevel > Sensitivity){
            char showx[1001];
            int xs, a;
            const int colwidth=120;
            
            for (a=0;a<colwidth;a++){
                showx[a] = '.';
            }
            showx[colwidth] = '\0';
            xs = (Trig.x*colwidth)/(1920);
            if (xs < 0) xs = 0;
            if (xs > colwidth-3) xs = colwidth-3;
           
            showx[xs] = '#';
            showx[xs+1] = '#';
            printf("%s %d,%d\n",showx, Trig.x, Trig.y);
			
			GeometryConvert(&Trig);
            
            #ifndef _WIN32
                if (UdpDest[0]) SendUDP(Trig.x, Trig.y, Trig.DiffLevel);
            #endif
        }
        
        Raspistill_restarted = 0;
    }

    if (LastPics[2].Image != NULL){
        // Third picture now falls out of the window.  Free it and delete it.
        
        if (SinceMotionFrames > 100){ // adjust
            // If it's been 200 images ince we saw motion, save this image
            // as a background image for later mouse detection.
            //printf("Save image as background");
            if (NoMousePic.Image != NULL){
                free(NoMousePic.Image);
            }
            NoMousePic = LastPics[2];
        }else{
            free(LastPics[2].Image);
        }
    }
    
    if (DeleteProcessed){
        //printf("Delete %s\n",LastPics[2].Name);
        unlink(LastPics[2].Name);
    }

    return LastPics[0].IsMotion;
}

//-----------------------------------------------------------------------------------
// Process a whole directory of files.
//-----------------------------------------------------------------------------------
static int NumProcessed;
static int DoDirectoryFunc(char * Directory, int DeleteProcessed)
{
    DirEntry_t * FileNames;
    int NumEntries;
    int a;
    int ReadExif;
    int SawMotion;
    
    SawMotion = 0;

    FileNames = GetSortedDir(Directory, &NumEntries);
    if (FileNames == NULL) return 0;
    if (NumEntries == 0) return 0;

    ReadExif = 1;
    NumProcessed = 0;
    for (a=0;a<NumEntries;a++){
        // Don't redo old pictures that we have looked at, but
        // not yet deleted because we may still need them.
        if (strcmp(LastPics[0].Name+LastPics[0].nind, FileNames[a].FileName) == 0
           || strcmp(LastPics[1].Name+LastPics[1].nind, FileNames[a].FileName) == 0){
            // Zero out file name to indicate skip this one.   
            FileNames[a].FileName[0] = 0;
        }
    }        

    
    for (a=0;a<NumEntries;a++){
        LastPic_t NewPic;
        struct stat statbuf;
        char * ThisName;
        int l;
       
        // Check that name ends in ".jpg", ".jpeg", or ".JPG", etc...
        ThisName = FileNames[a].FileName;
        if (ThisName[0] == 0) continue; // We already did this one.
        
        l = strlen(ThisName);
        if (l < 5) continue;
        if (ThisName[l-1] != 'g' && ThisName[l-1] != 'G') continue;
        if (ThisName[l-2] == 'e' || ThisName[l-2] == 'E') l-= 1;
        if (ThisName[l-2] != 'p' && ThisName[l-2] != 'P') continue;
        if (ThisName[l-3] != 'j' && ThisName[l-3] != 'J') continue;
        if (ThisName[l-4] != '.') continue;
        //printf("use: %s\n",ThisName);
        
        strcpy(NewPic.Name, CatPath(Directory, ThisName));
        NewPic.nind = strlen(Directory)+1;

        NewPic.Image = LoadJPEG(NewPic.Name, ScaleDenom, 0, ReadExif);
        if (NewPic.Image == NULL){
            fprintf(Log, "Failed to load %s\n",NewPic.Name);
            if (DeleteProcessed){
                // Raspberry pi timelapse mode may at times dump a corrupt
                // picture at the end of timelapse mode.  Just delete and go on.
                unlink(NewPic.Name);
            }
            continue;
        }
		ReadExif = 0; // Only read exif for first image.

        if (stat(NewPic.Name, &statbuf) == -1) {
            perror(NewPic.Name);
            exit(1);
        }
        NewPic.mtime = (unsigned)statbuf.st_mtime;
        LastPic_mtime = NewPic.mtime;

        SawMotion += ProcessImage(&NewPic, DeleteProcessed);

        NumProcessed += 1;
    }

    FreeDir(FileNames, NumEntries); // Free up the whole directory structure.
    FileNames = NULL;

    //if (SawMotion){
    //    run_blink_program();
    //}

    return SawMotion;
}


//-----------------------------------------------------------------------------------
// Process a whole directory of jpeg files.
//-----------------------------------------------------------------------------------
int DoDirectory(char * Directory)
{
    int a,b;
    Raspistill_restarted = 0;

    for (;;){
        a = DoDirectoryFunc(Directory, FollowDir);
        if (FollowDir){
            b = manage_raspistill(NumProcessed);
            if (b) Raspistill_restarted = 1;
            if (LogToFile[0] != '\0') LogFileMaintain();
            //sleep(1);
            usleep(100000);
        }else{
            break;
        }
    }
    return a;
}

//-----------------------------------------------------------------------------------
// Process a whole directory of video files.
//-----------------------------------------------------------------------------------
int DoDirectoryVideos(char * DirName)
{
    int a,ret;
    int infileindex;
    static int seq;
    Raspistill_restarted = 0;
    infileindex = strstr(VidDecomposeCmd, "<infile>")-VidDecomposeCmd;
    
    if (infileindex <= 0){
        fprintf(stderr, "Must specify '<infile>' as part of videodecomposecmd\n");
        exit(-1);
    }

    for (;;){
        DirEntry_t * FileNames;
        int NumEntries;
        char VidFileName[200];
        char FFCmd[300];
        int Saw_motion;
        NumProcessed = 0;
        
        FileNames = GetSortedDir(DirName, &NumEntries);
        if (FileNames == NULL){
            fprintf(stderr, "Could not read dir %s\n",DirName);
            return 0;
        } 
        
        if (NumEntries > 1) fprintf(Log,"%d files to process\n",NumEntries);
        for (a=0;a<NumEntries;a++){
            time_t now, age;
            time(&now);
            age = now-FileNames[a].ATime;
            fprintf(Log, "Video '%s' aged %ld ",FileNames[a].FileName, age);
            if (age < 6){
                fprintf(Log,"(Wait)\n");
                continue;
            }else{
                fprintf(Log,"Process...\n");
            }
            
            strcpy(VidFileName, CatPath(DirName, FileNames[a].FileName));
            strncpy(FFCmd, VidDecomposeCmd, infileindex);
            FFCmd[infileindex] = 0;
            strcpy(FFCmd+infileindex, VidFileName);
            strcat(FFCmd, VidDecomposeCmd+infileindex+8);
            sprintf(FFCmd+strlen(FFCmd), " %s/sf%02d_%%02d.jpg",TempDirName, seq);
            if (++seq >= 100) seq = 0;
            
            errno = 0;
            ret = system(FFCmd);
            if (ret || errno){
                // A command can however fail without errno getting set or system returning an error.
                if (errno) perror("system");
                printf("Error on command %s\n",FFCmd);
                continue;
            }
            
            // Now should have some files in temp dir.
            Saw_motion = DoDirectoryFunc(TempDirName, 1);
            if (Saw_motion){
				char * DstName;
				char * Ext;
                fprintf(Log,"Vid has motion %d\n", Saw_motion);
                // Make filename, but don't copy the file.
                DstName = BackupImageFile(VidFileName, Saw_motion,1);
				
				Ext = strstr(DstName, ".h264");
				if (Ext) strcpy(Ext, ".mp4"); // Change xtension to .mp4
				
				if (DstName){
					sprintf(FFCmd,"MP4Box -add %s \"%s\"",VidFileName, DstName);
					errno = 0;
					ret = system(FFCmd);
					if (ret || errno){
						if (errno) perror("system");
						printf("Error on command %s\n",FFCmd);
						continue;
					}
				}
            }
            
            if (FollowDir){
                //printf("Delete video %s\n",VidFileName);
                unlink(VidFileName);
            }
        }
        FreeDir(FileNames, NumEntries); // Free up the whole directory structure.
        
        if (FollowDir){
            int b = manage_raspistill(NumProcessed);
            if (b) Raspistill_restarted = 1;
            if (LogToFile[0] != '\0') LogFileMaintain();
            sleep(1);
        }else{
            break;
        }
        
    }
    return a;
}

static void ScaleRegion (Region_t * Reg, int Denom)
{
    Reg->x1 /= Denom;
    Reg->x2 /= Denom;
    Reg->y1 /= Denom;
    Reg->y2 /= Denom;
}


//-----------------------------------------------------------------------------------
// The main program.
//-----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    int file_index, a, argn;

    Log = stdout;

    printf("Imgcomp version 0.9 (Nov 2018) by Matthias Wandel\n\n");

    progname = argv[0];

    // Reset to default parameters.
    ScaleDenom = 4;
    DoDirName[0] = '\0';
    Sensitivity = 10;
	MsPerFrame = 250;
    Regions.DetectReg.x1 = 0;
    Regions.DetectReg.x2 = 1000000;
    Regions.DetectReg.y1 = 0;
    Regions.DetectReg.y2 = 1000000;
    Regions.NumExcludeReg = 0;
    TimelapseInterval = 0;
    SaveDir[0] = 0;
    strcpy(SaveNames, "%m%d/%H/%m%d-%H%M%S");

    for (argn = 1; argn < argc; argn++) {
        //printf("argn = %d\n",argn);
        char * arg;
        arg = argv[argn];
        if (strcmp(arg, "-h") == 0){
            usage();
            exit (-1);
        }
    }


    // First read the configuration file.
    read_config_file();

    // Get command line arguments (which may override configuration file)
    file_index = parse_switches(argc, argv, 0);
    
    if (LogToFile[0] != '\0'){
        Log = NULL;
        LogFileMaintain();
    }
    
    #ifndef _WIN32
    if (UdpDest[0]) InitUDP(UdpDest);
    #endif

    // Adjust region of interest to scale.
    ScaleRegion(&Regions.DetectReg, ScaleDenom);
    for (a=0;a<Regions.NumExcludeReg;a++){
        ScaleRegion(&Regions.ExcludeReg[a], ScaleDenom);
    }

    if (DoDirName[0]) printf("    Source directory = %s, follow=%d\n",DoDirName, FollowDir); 
    if (SaveDir[0]) printf("    Save to dir %s\n",SaveDir);
    if (TimelapseInterval) printf("    Timelapse interval %d seconds\n",TimelapseInterval);

    if (DiffMapFileName[0]){
        MemImage_t *MapPic;
        printf("    Diffmap file: %s\n",DiffMapFileName);
        if (Regions.DetectReg.x1 || Regions.DetectReg.y1
                || (Regions.DetectReg.x2 <  100000) || (Regions.DetectReg.y2 < 100000)){
            fprintf(stderr, "Specify diff map or detect regions, but not both\n");
            exit(-1);
        }

        if (Regions.NumExcludeReg){
            fprintf(stderr, "Specify diff map or exclude regions, but not both\n");
            exit(-1);
        }

        MapPic  = LoadJPEG(DiffMapFileName, ScaleDenom, 0, 0);
        if (MapPic == 0) exit(-1); // error is already reported.

        ProcessDiffMap(MapPic);
        free(MapPic);
    }
    
    
    // These directories are likely to be on ramdisk, so they may need re-creating.
    if (FollowDir) EnsurePathExists(DoDirName,0);
    if (TempDirName[0]) EnsurePathExists(TempDirName,0);
        
    
 
    if (DoDirName[0] && file_index == argc){
        // if dodir is specified in config file, but files are specified
        // on the command line, do the files instead.
        if (!VidMode){
            DoDirectory(DoDirName);
        }else{
            if (TempDirName[0] == 0){
                fprintf(stderr, "must specify tempdir for video mode\n");
                exit(-1);
            }
            DoDirectoryVideos(DoDirName);
        }   
    }

    if (argc-file_index == 2){
        MemImage_t *pic1, *pic2;
        
        printf("load %s\n",argv[file_index]);
        pic1 = LoadJPEG(argv[file_index], ScaleDenom, 0, 0);
        printf("\nload %s\n",argv[file_index+1]);
        pic2 = LoadJPEG(argv[file_index+1], ScaleDenom, 0, 0);
        if (pic1 && pic2){
            Verbosity = 2;
            ComparePix(pic1, pic2, 0, "diff.ppm");
        }
        free(pic1);
        free(pic2);
    }else{
        int a;
        MemImage_t * pic;

        for (a=file_index;a<argc;a++){

            printf("input file %s\n",argv[a]);
            // Load file into memory.

            pic = LoadJPEG(argv[a], 4, 0, 0);
            if (pic) WritePpmFile("out.ppm",pic);
        }
    }
    return 0;
}

// Features to consider adding:
//--------------------------------------------------------
// Polling same file mode (for use with  uvccapture)
// Dynamic thresholding if too much is happening?

