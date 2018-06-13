#include "TStopwatch.h"
#include "TRandom.h"

#include <memory>
#include <vector>
#include <chrono>
#include <thread> // For sleep_until()

#include <unistd.h> // For usleep
#include <time.h>   // For clock_gettime
#include "czmq.h"

#include "TriggerPrimitive.h"
#include "PrimitiveSource.h"

#include "trace.h"

 // A convenient time for DUNE hardware, here one of JJs 1024-tick blocks, the stride is set as a multiple of this
using TStepTime = std::chrono::duration<int64_t,std::ratio<8,15625>>;
// We can compare times from this clock with times on other machines, but it may jump
using TSystemTimePoint = std::chrono::time_point<std::chrono::system_clock>;
// Does not jump, but we can't set it to what we want (that is why it doesn't jump :)), we can add/subtract durations from it
using TSteadyTimePoint = std::chrono::time_point<std::chrono::steady_clock>;

//======================================================================
// What is the time step corresponding to the given timestamp? ie,
// how many TStepTime's have passed between the epoch and
// timestamp occurring
size_t timestepForTimestamp(int64_t timestamp)
{
    TSystemTimePoint stpTimestamp{std::chrono::duration_cast<TSteadyTimePoint::duration>(std::chrono::nanoseconds(timestamp))};
    // How many units of TStepTime have passed at `timestamp`?
    size_t nSteps=std::chrono::duration_cast<TStepTime>(stpTimestamp.time_since_epoch()).count();
    return nSteps;
}

//======================================================================
int64_t lastTimestampForTimestep(size_t timestep)
{
    TStepTime steps{timestep+1};
    return std::chrono::duration_cast<std::chrono::nanoseconds>(steps).count()-1;
}

//======================================================================
// Get the real time in nanoseconds
unsigned long long now()
{
    timespec tp_end;
    clock_gettime(CLOCK_MONOTONIC, &tp_end);
    const unsigned long long billion=1000000000;
    return billion*tp_end.tv_sec+tp_end.tv_nsec;
}

//======================================================================
void usage()
{
    printf("Usage: primsim [-s] [-a APA#] -n nTimeSteps\n");
    exit(1);
}

//======================================================================
int main(int argc, char** argv)
{
    bool speedtest = false;
    int c=0;
    unsigned int nTimeSteps=0;
    int apa=0;
    float multiplier=1;

    opterr = 0;
    while ((c = getopt (argc, argv, "sn:a:m:")) != -1){
        switch (c)
        {
        case 's':
            speedtest = true;
            break;
        case 'n':
            nTimeSteps=atoi(optarg);
            break;
        case 'a':
            apa=atoi(optarg);
            break;
        case 'm':
            multiplier=atof(optarg);
            break;
        default:
            usage();
        }
    }

    if(nTimeSteps==0){
        usage();
    }

    const int stepsPerIteration=(1<<8); // Only call the primitive generation every ...th iteration

    std::vector<PrimitiveSource*> sources;
    // Rate comes from calc_rates.C with the FH15 setting.  Mean and
    // sigma of the Gaussian come from a by-hand fit to the plot made
    // by cluster_dbscan.C, FH15

    sources.push_back(new NoiseSource(multiplier*7.18e-03*stepsPerIteration, -260, 150));
    // Ar39 looks a lot like noise. Rates and shape as above
    sources.push_back(new NoiseSource(multiplier*4.32e-03*stepsPerIteration, 9, 96));
    // A library of Marley events
    sources.push_back(new LibrarySource(multiplier*1e-3*stepsPerIteration, "marley_hits.txt"));

    // The list of primitives waiting to be emitted. The first index
    // is the escalator step that
    // the primitive goes in (not using a shared_ptr here because we
    // want to memcpy the contents of each step in sendPrimitives()
    // later)
    std::vector<std::vector<TriggerPrimitive>> primsByStep;

    const int primBufferSize=1<<10;
    // prims.resize(primBufferSize);
    primsByStep.reserve(primBufferSize);

    gRandom->SetSeed(101*apa);

    // Infinite high-water mark for sending, so we don't lose things
    // when the receiver can't keep up. A better model might be to
    // actually drop those packets and have the receiver request
    // resend, but we'll wait till later to do that
    zsys_set_sndhwm(0);
    zsock_t* publisher = zsock_new(ZMQ_RADIO);
    zsock_connect(publisher, "udp://127.0.0.1:5601");

    unsigned int totalPrims=0;
    unsigned int sentPrims=0;
    unsigned int sentEOTs=0;
    uint64_t last_sent_tstamp=0;
    TStopwatch ts;
    ts.Start();

    // TODO: Work out how to make it explicit that I want this time in
    // nanoseconds (it is, but it would be nice to be clear)
    auto start_systemtime=std::chrono::system_clock::now().time_since_epoch().count();
    uint64_t prevTimestep=timestepForTimestamp(start_systemtime);

    for(uint64_t curTick=0; curTick<nTimeSteps; ++curTick){
        // The timestamp in nanoseconds for the current tick
        int64_t curTimestamp=start_systemtime+500*curTick;

        //--------------------------------------------------------------
        // Is this a step on which we should generate new primitives?
        // We only do this every so often because otherwise we spend
        // time throwing lots of Poisson variates that are all zero
        if((curTick & (stepsPerIteration-1))==0){
            // Loop over all the sources we know about and have each one produce primitives
            std::vector<std::shared_ptr<TriggerPrimitive>> sourcePrims;
            for(auto const& source : sources){
                source->addPrimitives(curTick, stepsPerIteration, sourcePrims);
            }
            totalPrims+=sourcePrims.size();
            // Put each primitive into the appropriate spot (by timestamp)
            // in the master list of primitives
            for(auto const& prim: sourcePrims){
                // The time the trigger primitive would be emitted, in
                // ticks relative to the start of the primsim loop
                uint64_t emitTime=prim->timestamp+prim->time_over_threshold;
                // The time the trigger primitive would be emitted, in system time in nanoseconds
                uint64_t emitTimestamp=start_systemtime+500*emitTime;
                prim->timestamp=emitTimestamp;
                // Check that emit time is not in the past
                if(emitTime<curTick){
                    std::cout << "Emit time is " << emitTime << " with current time " << curTick << std::endl;
                }

                const int emitBufferIndexStep=timestepForTimestamp(emitTimestamp) & (primBufferSize-1);
                if(emitBufferIndexStep>=primBufferSize){
                    std::cout << "emitBufferIndexStep is " << emitBufferIndexStep << " while primBufferSize is " << primBufferSize << std::endl;
                }
                primsByStep[emitBufferIndexStep].push_back(*prim);

            }
        }

        //--------------------------------------------------------------
        // Are we at the end of an escalator time step? If so, send
        // out a trigger primitive with the sequence number set to all
        // 1s to indicate to downstream that we're done
        uint64_t curTimestep=timestepForTimestamp(curTimestamp);
        if(curTimestep!=prevTimestep){
            // Send the set of primitives as a single ZeroMQ message
            const int index=prevTimestep & (primBufferSize-1);
            std::vector<TriggerPrimitive>& ps=primsByStep[index];
            sendPrimitives(publisher, ps);
            sentPrims+=ps.size();
            ps.clear();
            ps.reserve(100);
            // Set the timestamp of the EOT packet to the latest
            // timestamp that will be in the timestep that it's
            // for. This way, the trigger farm can easily associate it
            // with the correct timestep, and it will sort after all
            // of the packets in that step
            int64_t lastTS=lastTimestampForTimestep(prevTimestep);
            TriggerPrimitive endPrim(lastTS, apa, 0, 0, 0, std::numeric_limits<uint32_t>::max());
            endPrim.send(publisher);

            ++sentEOTs;
            // reset the sequence number for the next step
            prevTimestep=curTimestep;
        }

        //--------------------------------------------------------------
        // Every so often, check whether we're ahead of where we should be, and if so, slow down a bit
        constexpr int slowdown_steps=256;
        if( (!speedtest) && ((curTick & (slowdown_steps-1))==0) && (curTick!=0) ){
            TSystemTimePoint stp{std::chrono::nanoseconds(curTimestamp)};
            TSystemTimePoint stpNow=std::chrono::system_clock::now();
            if(stpNow-stp > std::chrono::milliseconds(2)){
                // std::cout <<  "primsim: Behind by " << std::chrono::duration_cast<std::chrono::microseconds>(stpNow-stp).count() << "us at step " << curTick << std::endl;
            }
            else{
                std::this_thread::sleep_until(stp);
            }
        } // end if( (!speedtest) && ((curTime & (slowdown_steps-1))==0) && (curTime!=0) )
    }
    ts.Stop();

    printf("primsim: Simulated APA %d. %.1fk steps in %.2fs (%.1fk steps/s, %.1fx realtime) with %d primitives sent (%.0fk prim/s)\n",
           apa, 1e-3*nTimeSteps, ts.RealTime(), 1e-3*nTimeSteps/ts.RealTime(),
           nTimeSteps/ts.RealTime()/SAMPLING_RATE,
           sentPrims, 1e-3*sentPrims/ts.RealTime());
    printf("primsim: sent %d EOT packets. last sent timestamp: %lu\n", sentEOTs, last_sent_tstamp);
    zsock_destroy(&publisher);

    return 0;
}

// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:
