// Based on mausc.c by Tinsel Phipps.
// v1.0 Roger Siddons
// v2.0 Roger Siddons: Flag clusters asap, fix segfaults, optional headers
// v3.0 Roger Siddons: Remove lib dependencies & commfree
// v4.0 Kill process argv[1] when idle for 30 seconds.
// v4.1 Fix averaging overflow
// v4.2 Unblock the alarm signal so the job actually finishes.
// Public domain. Requires libsndfile
// Detects commercial breaks using clusters of audio silences

#include <cstdlib>
#include <cmath>
#include <cerrno>
#include <climits>
#include <deque>
#include <sndfile.h>
#include <unistd.h>
#include <signal.h>

typedef unsigned frameNumber_t;
typedef unsigned frameCount_t;

// Output to python wrapper requires prefix to indicate level
#define DELIMITER "@" // must correlate with python wrapper
char prefixdebug[7] = "debug" DELIMITER;
char prefixinfo[6]  = "info" DELIMITER;
char prefixerr[5]   = "err" DELIMITER;
char prefixcut[5]   = "cut" DELIMITER;

void error(const char* mesg, bool die = true)
{
    printf("%s%s\n", prefixerr, mesg);
    if (die)
        exit(1);
}

pid_t tail_pid = 0;
void watchdog(int sig)
{
    if (0 != tail_pid)
        kill(tail_pid, SIGTERM);
}

namespace Arg
// Program argument management
{
const float kvideoRate = 25.0;  // sample rate in fps (maps time to frame count)
const frameCount_t krateInMins = kvideoRate * 60; // frames per min
unsigned useThreshold;          // Audio level of silence
frameCount_t useMinQuiet;       // Minimum length of a silence to register
unsigned useMinDetect;          // Minimum number of silences that constitute an advert
frameCount_t useMinLength;      // adverts must be at least this long
frameCount_t useMaxSep;         // silences must be closer than this to be in the same cluster
frameCount_t usePad;            // padding for each cut

void usage()
{
    error("Usage: silence <tail_pid> <threshold> <minquiet> <mindetect> <minlength> <maxsep> <pad>", false);
    error("<tail_pid> : (int)    Process ID to be killed after idle timeout.", false);
    error("<threshold>: (float)  silence threshold in dB.", false);
    error("<minquiet> : (float)  minimum time for silence detection in seconds.", false);
    error("<mindetect>: (float)  minimum number of silences to constitute an advert.", false);
    error("<minlength>: (float)  minimum length of advert break in seconds.", false);
    error("<maxsep>   : (float)  maximum time between silences in an advert break in seconds.", false);
    error("<pad>      : (float)  padding for each cut point in seconds.", false);
    error("AU format audio is expected on stdin.", false);
    error("Example: silence 4567 -75 0.1 5 60 90 1 < audio.au");
}

void parse(int argc, char **argv)
// Parse args and convert to useable values (frames)
{
    if (8 != argc)
        usage();

    float argThreshold; // db
    float argMinQuiet; // secs
    float argMinDetect;
    float argMinLength; // secs
    float argMaxSep; // secs
    float argPad; // secs

    /* Load options. */
    if (1 != sscanf(argv[1], "%d", &tail_pid))
        error("Could not parse tail_pid option into a number");
    if (1 != sscanf(argv[2], "%f", &argThreshold))
        error("Could not parse threshold option into a number");
    if (1 != sscanf(argv[3], "%f", &argMinQuiet))
        error("Could not parse minquiet option into a number");
    if (1 != sscanf(argv[4], "%f", &argMinDetect))
        error("Could not parse mindetect option into a number");
    if (1 != sscanf(argv[5], "%f", &argMinLength))
        error("Could not parse minlength option into a number");
    if (1 != sscanf(argv[6], "%f", &argMaxSep))
        error("Could not parse maxsep option into a number");
    if (1 != sscanf(argv[7], "%f", &argPad))
        error("Could not parse pad option into a number");

    /* Scale threshold to integer range that libsndfile will use. */
    useThreshold = rint(INT_MAX * pow(10, argThreshold / 20));

    /* Scale times to frames. */
    useMinQuiet  = ceil(argMinQuiet * kvideoRate);
    useMinDetect = (int)argMinDetect;
    useMinLength = ceil(argMinLength * kvideoRate);
    useMaxSep    = rint(argMaxSep * kvideoRate + 0.5);
    usePad       = rint(argPad * kvideoRate + 0.5);

    printf("%sThreshold=%.1f, MinQuiet=%.2f, MinDetect=%.1f, MinLength=%.1f, MaxSep=%.1f, Pad=%.2f\n",
           prefixdebug, argThreshold, argMinQuiet, argMinDetect, argMinLength, argMaxSep, argPad);
    printf("%sFrame rate is %.2f, Detecting silences below %d that last for at least %d frames\n",
           prefixdebug, kvideoRate, useThreshold, useMinQuiet);
    printf("%sClusters are composed of a minimum of %d silences closer than %d frames and must be\n",
           prefixdebug, useMinDetect, useMaxSep);
    printf("%slonger than %d frames in total. Cuts will be padded by %d frames\n",
           prefixdebug, useMinLength, usePad);
    printf("%s< preroll, > postroll, - advert, ? too few silences, # too short, = comm flagged\n", prefixdebug);
    printf("%s           Start - End    Start - End      Duration         Interval    Level/Count\n", prefixinfo);
    printf("%s          frame - frame (mmm:ss-mmm:ss) frame (mm:ss.s)  frame (mmm:ss)\n", prefixinfo);
}
}

class Silence
// Defines a silence
{
public:
    enum state_t {progStart, detection, progEnd};
    static const char state_log[3];

    const state_t state;       // type of silence
    const frameNumber_t start; // frame of start
    frameNumber_t end;         // frame of end
    frameCount_t length;       // number of frames
    frameCount_t interval;     // frames between end of last silence & start of this one
    double power;              // average power level

    Silence(frameNumber_t _start, double _power = 0, state_t _state = detection)
        : state(_state), start(_start), end(_start), length(1), interval(0), power(_power) {}

    void extend(frameNumber_t frame, double _power)
    // Define end of the silence
    {
        end = frame;
        length = frame - start + 1;
        // maintain running average power: = (oldpower * (newlength - 1) + newpower)/ newlength
        power += (_power - power)/length;
    }
};
// c++0x doesn't allow initialisation within class
const char Silence::state_log[3] = {'<', ' ', '>'};

class Cluster
// A cluster of silences
{
private:
    void setState()
    {
        if (this->start->start == 1)
            state = preroll;
        else if (this->end->state == Silence::progEnd)
            state = postroll;
        else if (length < Arg::useMinLength)
            state = tooshort;
        else if (silenceCount < Arg::useMinDetect)
            state = toofew;
        else
            state = advert;
    }

public:
    // tooshort..unset are transient states - they may be updated, preroll..postroll are final
    enum state_t {tooshort, toofew, unset, preroll, advert, postroll};
    static const char state_log[6];

    static frameNumber_t completesAt; // frame where the most recent cluster will complete

    state_t state;          // type of cluster
    const Silence* start;   // first silence
    Silence* end;           // last silence
    frameNumber_t padStart, padEnd; // padded cluster start/end frames
    unsigned silenceCount;  // number of silences
    frameCount_t length;    // number of frames
    frameCount_t interval;  // frames between end of last cluster and start of this one

    Cluster(Silence* s) : state(unset), start(s), end(s), silenceCount(1), length(s->length), interval(0)
    {
        completesAt = end->end + Arg::useMaxSep; // finish cluster <maxsep> beyond silence end
        setState();
        // pad everything except pre-rolls
        padStart = (state == preroll ? 1 : start->start + Arg::usePad);
    }

    void extend(Silence* _end)
    // Define end of a cluster
    {
        end = _end;
        silenceCount++;
        length = end->end - start->start + 1;
        completesAt = end->end + Arg::useMaxSep; // finish cluster <maxsep> beyond silence end
        setState();
        // pad everything except post-rolls
        padEnd = end->end - (state == postroll ? 0 : Arg::usePad);
    }
};
// c++0x doesn't allow initialisation within class
const char Cluster::state_log[6] = {'#', '?', '.', '<', '-', '>'};
frameNumber_t Cluster::completesAt = 0;

class ClusterList
// Manages a list of detected silences and a list of assigned clusters
{
protected:
    // list of detected silences
    std::deque<Silence*> silence;

    // list of deduced clusters of the silences
    std::deque<Cluster*> cluster;

public:
    Silence* insertStartSilence()
    // Inserts a fake silence at the front of the silence list
    {
        // create a single frame silence at frame 1 and insert it at front
        Silence* ref = new Silence(1, 0, Silence::progStart);
        silence.push_front(ref);
        return ref;
    }

    void addSilence(Silence* newSilence)
    // Adds a silence detection to the end of the silence list
    {
        // set interval between this & previous silence/prog start
        newSilence->interval = newSilence->start
                - (silence.empty() ? 1 : silence.back()->end - 1);
        // store silence
        silence.push_back(newSilence);
    }

    void addCluster(Cluster* newCluster)
    // Adds a cluster to end of the cluster list
    {
        // set interval between new cluster & previous one/prog start
        newCluster->interval = newCluster->start->start
                - (cluster.empty() ? 1 : cluster.back()->end->end - 1);
        // store cluster
        cluster.push_back(newCluster);
    }
};

Silence* currentSilence; // the silence currently being detected/built
Cluster* currentCluster; // the cluster currently being built
ClusterList* clist;      // List of completed silences & clusters

void report(const char* err,
            const char type,
            const char* msg1,
            const frameNumber_t start,
            const frameNumber_t end,
            const frameNumber_t interval,
            const int power)
// Logs silences/clusters/cuts in a standard format
{
    frameCount_t duration = end - start + 1;

    printf("%s%c %7s %6d-%6d (%3d:%02ld-%3d:%02ld), %4d (%2d:%04.1f), %5d (%3d:%02ld), [%7d]\n",
           err, type, msg1, start, end,
           (start+13) / Arg::krateInMins, lrint(start / Arg::kvideoRate) % 60,
           (end+13) / Arg::krateInMins, lrint(end / Arg::kvideoRate) % 60,
           duration, (duration+1) / Arg::krateInMins, fmod(duration / Arg::kvideoRate, 60),
           interval, (interval+13) / Arg::krateInMins, lrint(interval / Arg::kvideoRate) % 60, power);
}

void processSilence()
// Process a silence detection
{
    // ignore detections that are too short
    if (currentSilence->state == Silence::detection && currentSilence->length < Arg::useMinQuiet)
    {
        // throw it away
        delete currentSilence;
        currentSilence = NULL;
    }
    else
    {
        // record new silence
        clist->addSilence(currentSilence);

        // assign it to a cluster
        if (currentCluster)
        {
            // add to existing cluster
            currentCluster->extend(currentSilence);
        }
        else if (currentSilence->interval <= Arg::useMaxSep) // only possible for very first silence
        {
            // First silence is close to prog start so extend cluster to the start
            // by inserting a fake silence at prog start and starting the cluster there
            currentCluster = new Cluster(clist->insertStartSilence());
            currentCluster->extend(currentSilence);
        }
        else
        {
            // this silence is the start of a new cluster
            currentCluster = new Cluster(currentSilence);
        }
        report(prefixdebug, currentSilence->state_log[currentSilence->state], "Silence",
               currentSilence->start, currentSilence->end,
               currentSilence->interval, currentSilence->power);

        // silence is now owned by the list, start looking for next
        currentSilence = NULL;
    }
}

void processCluster()
// Process a completed cluster
{
    // record new cluster
    clist->addCluster(currentCluster);

    report(prefixinfo, currentCluster->state_log[currentCluster->state], "Cluster",
           currentCluster->start->start, currentCluster->end->end,
           currentCluster->interval, currentCluster->silenceCount);

    // only flag clusters at final state
    if (currentCluster->state > Cluster::unset)
        report(prefixcut, '=', "Cut", currentCluster->padStart, currentCluster->padEnd, 0, 0);

    // cluster is now owned by the list, start looking for next
    currentCluster = NULL;
}

int main(int argc, char **argv)
// Detect silences and allocate to clusters
{
    // Remove logging prefixes if writing to terminal
    if (isatty(1))
        prefixcut[0] = prefixinfo[0] = prefixdebug[0] = prefixerr[0] = '\0';

    // flush output buffer after every line
    setvbuf(stdout, NULL, _IOLBF, 0);

    Arg::parse(argc, argv);

    /* Check the input is an audiofile. */
    SF_INFO metadata;
    SNDFILE* input = sf_open_fd(STDIN_FILENO, SFM_READ, &metadata, SF_FALSE);
    if (NULL == input) {
        error("libsndfile error:", false);
        error(sf_strerror(NULL));
    }

    /* Allocate data buffer to contain audio data from one video frame. */
    const size_t frameSamples = metadata.channels * metadata.samplerate / Arg::kvideoRate;

    int* samples = (int*)malloc(frameSamples * sizeof(int));
    if (NULL == samples)
        error("Couldn't allocate memory");

    // create silence/cluster list
    clist = new ClusterList();

    // Kill head of pipeline if timeout happens.
    signal(SIGALRM, watchdog);
    sigset_t intmask;
    sigemptyset(&intmask);
    sigaddset(&intmask, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &intmask, NULL);
    alarm(30);

    // Process the input one frame at a time and process cuts along the way.
    frameNumber_t frames = 0;
    while (frameSamples == static_cast<size_t>(sf_read_int(input, samples, frameSamples)))
    {
        alarm(30);
        frames++;

        // determine average audio level in this frame
        unsigned long long avgabs = 0;
        for (unsigned i = 0; i < frameSamples; i++)
            avgabs += abs(samples[i]);
        avgabs = avgabs / frameSamples;

        // check for a silence
        if (avgabs < Arg::useThreshold)
        {
            if (currentSilence)
            {
                // extend current silence
                currentSilence->extend(frames, avgabs);
            }
            else // transition to silence
            {
                // start a new silence
                currentSilence = new Silence(frames, avgabs);
            }
        }
        else if (currentSilence) // transition out of silence
        {
            processSilence();
        }
        // in noise: check for cluster completion
        else if (currentCluster && frames > currentCluster->completesAt)
        {
            processCluster();
        }
    }
    // Complete any current silence (prog may have finished in silence)
    if (currentSilence)
    {
        processSilence();
    }
    // extend any cluster close to prog end
    if (currentCluster && frames <= currentCluster->completesAt)
    {
        // generate a silence at prog end and extend cluster to it
        currentSilence = new Silence(frames, 0, Silence::progEnd);
        processSilence();
    }
    // Complete any final cluster
    if (currentCluster)
    {
        processCluster();
    }
}

