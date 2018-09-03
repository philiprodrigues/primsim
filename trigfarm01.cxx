#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <algorithm> // For std::sort
#include <locale.h> // For setlocale
#include "TFarmCV.hxx"  // Also defines TFarmCVV

#include "TriggerPrimitive.h"

#include <czmq.h>

#include "trace.h"

// A mutex to hold when we want to printf/cout
std::mutex gPrintMutex;


//----------------------------------------------------------------------
// One packet (can store some max amount of data).  We will make a large
// number of these in the MemPool at the start of the program
template<size_t N>
class GenPacket {
public:
    uint32_t incoming_address;
    // static const size_t MAXSIZE_=6;   // Maxsize of an incoming packet, TODO set to 1536/sizeof(uint64_t), currently small for teating
    std::array<uint64_t, N> content_;
public:
    GenPacket() : content_{0xDEADBEEF} {}   // Constructor
};

typedef GenPacket<6> Packet;

//======================================================================
//--------------------------------------------------------------------
// Mempool.  A big vector of memory that can store packets that we
// allocate when the program starts.   The allocation and deallocation of this
// memory is mainly done by the StoreI and StoreB functions.
class MemPool {
public:
    static const size_t POOLSIZE_ = 1<<15;
    std::vector<Packet> pool_;
public:
    MemPool() : pool_(POOLSIZE_), BackPointer_(0), IPointer_(0), FrontPointer_(0) {}
    // 1: Where new items are placed when read in (then REC increments this)
    //    [Items waiting between step 1 and step 2 are said to be in storeI]
    std::atomic<size_t> BackPointer_;

    // 2: SRT sorts the items at ipointer_ (incrementing it as we go)
    //         and adding the entry number into the StoreB.
    // FirstPointer_ and LatestPointer_ are in each StoreMItem, i.e. one per step
    // 3: SRT adds the packet in the right place in the StoreMItem corresponding
    //    to the step.  The first packet to arrive is also stored in FirstPointer_
    //    and as each additional packet arrives, the LatestPointer_ variable is updated
    //    This allows us to deallocate in bulk when finished.
    std::atomic<size_t> IPointer_;
    // 4: CTL updates FrontPointer_ when a step gets to deallocation.  If BackPointer_
    //    moves around as far as FrontPointer_ we are in a bad shape as it means
    //    we are not fast enough and the mempool has filled up.  This
    //    should never happen if we bought enough memory and made our
    //    algorithms fast enough.
    std::atomic<size_t> FrontPointer_;

    // What is the distance from ptr1 to ptr2, accounting for
    // wraparound? (Not really a distance because it's not
    // commutative)
    size_t distance(size_t ptr1, size_t ptr2)
    {
        if(ptr2>=ptr1) return ptr2-ptr1;
        else return POOLSIZE_ + ptr2 - ptr1;
    }

    // Advance `ptr` by `distance` steps, accounting for wraparound.
    void advance(std::atomic<size_t>& ptr, size_t distance)
    {
        // TODO: Do we need to handle the case where someone else
        // modifies `ptr` between when we read and when we write?
        //
        // Perhaps using something like:
        // https://stackoverflow.com/questions/33554255/
        size_t newVal=(ptr.load()+distance) & (POOLSIZE_ - 1);
        ptr.store(newVal);
    }
};

//======================================================================
//------------------------------------------------------------------
// Store M: Variables related to a pipeline step.  This will be renamed the 'escalator' (and StoreMItem the EscalatorStep).

using TNanoTime = std::chrono::duration<int64_t,std::nano>;           // Nanoseconds
using TStepTime = std::chrono::duration<int64_t,std::ratio<8,15625>>; // A convenient time for DUNE hardware, here one of JJs 1024-tick blocks, the stride is set as a multiple of this
using TSystemTimePoint = std::chrono::time_point<std::chrono::system_clock>;   // We can compare times from this clock with times on other machines, but it may jump
using TSteadyTimePoint = std::chrono::time_point<std::chrono::steady_clock>;   // Does not jump, but we can't set it to what we want (that is why it doesn't jump :)), we can add/subtract durations from it

//======================================================================
class TrigDecision {
    // What to put in here?  Define time window and which APAs in ROI};
};

//======================================================================
enum class MItemState {
    Inactive,        // 0=Inactive (CTL has deactivted it, finished and waiting for reuse)
    Initialized,     // 1=Initialised, receiving  packets from SRT
    ReadyForAlgs,    // 2=ALGs allowed to start (all packets received or times-up for packet reception)
    AlgsFinished,    // 3=ALGs all finished (either they did it, or times-up for calculation),
                     //        so SRT is attempting sending trigger decisions out
    TriggersSent,    // 4=SRT has confirmation all trigger decisions sent
    Finished         // 5=Everything finished includng any SNB stuff [CTL may set state to 0 here]
                     // ...More states need adding when we put the SNB trigger in this framework.


};

//======================================================================
class StoreMItem {
public:
    // Overall state of this step.  Always filled in by CTL, this is really just so
    // CTL does not need to reevaluate every step every time the pipeline ticks on.
    std::atomic<MItemState> State_;
    TNanoTime::rep StepTime_;     // The time step of this step (filled in by CTL when State_ set to 1)
    int64_t NanoTime_;            // The time in nanosecs of this step (filled in by CTL when State_ set to 1)
    int32_t StartStep_;           // The index of this step at the start (may be the same as StepTime_ must think a bit more)

    //-------------------------------------------------------
    // Things filled by SRT process

    // Number of trigger sources, normally 750, start testing with less
    static unsigned int NSOURCE_;
    // One flag per source (i.e. per APA/WIB) set to 1 when packet (and
    // any extension packets) have arrived
    std::vector<int> NotMissingFlag_;

    // One pointer per APA/WIB set to -1 at start and then
    // to the entry in the mempool where the first packet is.
    // There may be extension packets linked from the first one
    // This value changes when the first of the packets
    // for this APA/WIB arrives.  NotMissingFlag_ gets set
    // when they have all arrived.  To start with, we ignore the
    // possibility of extension packets.
    std::vector<size_t> PacketPointer_;

    std::atomic<size_t> FirstPointer_;      // Which is the earliest pointer in mempool for this step.
    std::atomic<size_t> LatestPointer_;     // Which is the latest pointer in mempool for this step.

    // Increments each time we set a NotMissingFlag_, so when it is
    // equal to NSOURCE, we have all the packets.
    unsigned int NPointer_;
    bool added_;   // Local variable set when we add a packet.  At end of step,
                   // used to decide whether to increment SRTstatus.

    // A constant that we can set pointers to to indicate that they're uninitialized
    static constexpr size_t InvalidPtr=std::numeric_limits<size_t>::max();

    // A constant for "SRT finished with this step by receiving all the data for it"
    static constexpr int SRTDone=std::numeric_limits<int>::max();
    // A constant for "SRT finished with this step by timing out
    // (before receiving all the data for it)"
    static constexpr int SRTTimedOut=std::numeric_limits<int>::max()-1;
    std::atomic<int> SRTstatus;    // starts at 0, increment each step where more
                                   // things are not missing.  Set to SRTDone
                                   // when we give up looking for more.  Read by
                                   // ALG so it knows when more stuff is available to
                                   // look at and when `SRTDone`, ALG knows it gets no more.

    //-------------------------------------------------------
    // Things filled by ALG process

    // std::vector<TrigDecision> PrelimDecisions;  // Can have many triggeres in one pipeline step
    // std::atomic<int> ALGPrelimDone;             // Set by ALG.  Starts at 0, set to 1 when prelimDecisions_ filled
    std::vector<TrigDecision> FinalDecisions;      // Can have many triggeres in one pipeline step
    std::atomic<int> ALGFinalDone; // Set by ALG.  Starts at 0, set to 1 when FinalDecisions_ filled [We have reached state 4 when this is set]

    std::atomic<int> ALGFinished;  // Set to 1 by ALG when all done;
                                   // i.e. summary made and sent and
                                   // packets in mempool pointed to by
                                   // storeB released, for now this is
                                   // ignored, but TODO we will need
                                   // something like this when we add
                                   // the SN [We have reached state 9
                                   // when this is set]

    std::atomic<int> SRTDecisionFinalSent;  // Set to 1 by SRT when
                                            // decisions have been
                                            // sent and confirmed, so
                                            // SRT doesn't need this
                                            // pipeline step info
                                            // (store M and storeB)
                                            // any more [We have
                                            // reached state 9 when
                                            // this is set and ALG
                                            // finished is set]

    // When ALGfinished and STODecisionFinalSent are both set, we have
    // reached state 9.  This means this pipeline step is finished,
    // and the escalator step can be reinitialised and reused [by CTL]

    // Since there is exactly one ALG process per possible pipeline
    // step (i.e. per storeM), we put the pointer to the ALG thread,
    // and any other thread communications in the storeMItem
    std::thread AlgThread;   // Default contructing a thread is unusual (p 1213 of Stroustrup) but it can be a target of a move as done here in main
    TFarmCVV AlgReply;   // The ALG responds to the condition variable by copying it in here
public:
    StoreMItem()
        { this->Clear(); } // Constructor

    void Clear() {
        State_.store(MItemState::Inactive);
        StepTime_ = -1; NanoTime_ = -1; FirstPointer_ = InvalidPtr; LatestPointer_ = InvalidPtr;
        NPointer_ = 0;  added_ = false;
        SRTstatus.store(0);  ALGFinalDone.store(0);
        ALGFinished.store(0);  SRTDecisionFinalSent.store(0);
        // ALGPrelimDone.store(0); PrelimDecisions.clear();
        FinalDecisions.clear();   // Check .clear() is the correct function to use

        // Need to do this resize here, and not in the initializer
        // list, because NSOURCE_ is a command line parameter which
        // hasn't been set at the time the constructor for the global
        // is called
        PacketPointer_.clear();
        NotMissingFlag_.clear();
        PacketPointer_.reserve(NSOURCE_);
        NotMissingFlag_.reserve(NSOURCE_);

        for (unsigned int i=0; i<NSOURCE_; i++) {
            PacketPointer_.push_back(InvalidPtr);
            NotMissingFlag_.push_back(0);
        }
    }

};

// Need these so there's a 'definition' of the variables, otherwise we
// get 'undefined reference' linker errors
constexpr size_t StoreMItem::InvalidPtr;
unsigned int StoreMItem::NSOURCE_ = 0;

//======================================================================
// There is a StoreMItem for each pipeline step, stored in order in a ring in here
class StoreM {
public:
    static const int PLENGTH_ = 64;  // Length of store = How many pipeline steps tracked at one time  We want this to be quite big, e.g. 90 on a 96 thread machine. Must be a power of two, because it gets used to set StoreMWrap_
    std::vector<StoreMItem> Items_;   // Make the capacity of this PLENGTH_

    // The following are the interthread communications that are common to everyone, there is only once instance in the entire trigger farm.
    TFarmCVV RecReply;   // Likely never needed, except for a test generation mode to say when a pipeline step has been completely generated
    TFarmCVV SrtReply;   // Reply when SRT has responded to CtlCmd, which means it has sent the current response packet
    TFarmCVV CtlReply;   // This may never be needed.

    TFarmCVV SrtTimeoutFlag;  // The flag by which CTL tells SRT to
                              // give up on this step because it's
                              // timed out (SrtMustAdvance), and by
                              // which SRT responds that it has
                              // stopped modifying anything about the
                              // StoreMItem for the step it was
                              // working on (SrtAdvanced). Maybe this
                              // can be merged with SrtReply?
    static constexpr int SrtMustAdvance=2;
    static constexpr int SrtAdvanced=3;
    static constexpr int SrtAdvancedEmpty=4; // SRT advanced but hadn't found any packets for this step

    TFarmCVV CtlCmd;     // This is the command variable
    TFarmCV CtlCmdCV;    // This encapsultes the control_variable stuff so we can play with
    // using a real condition_variable or some waits. It has functions
    // we call to set/check all the TFarmCVVs (One command variable here,
    // and lots of reply variables (including the ALG ones in StoreMItem).

    TNanoTime LocalTimeShift_;                // A correction if the clock is shifted a bit
    TSystemTimePoint TimePointSystemEpoch_;   // Time when the program started by the system clock
    TSteadyTimePoint TimePointSteadyEpoch_;   // Time corresponding to TimePointSystemEpoch in the steady clock

    // The number of timesteps since the UNIX epoch that had elapsed
    // when this process started. Just needed so we can subtract it in
    // timestepForTimestamp and thereby number our timesteps starting
    // at ~0
    int64_t StepsAtEpoch_;
    // TODO: For now this is OK, but need to eventually set
    // the Epoch from configuration so it is the same everywhere,
    // (We can do it from this for the inputs, but needed if there
    // is more than one trigger processor farm node)
    int64_t ThisTimeStep_;                    // Current step time count [set by CTL]

public:
    StoreM() : Items_(PLENGTH_) {}   // Constructor

    // What is the time step corresponding to the given timestamp? ie,
    // how many TStepTime's have passed between the epoch and
    // timestamp occurring? The use of time_since_epoch() means our
    // steps start on 1024-tick boundaries so we keep in sync with
    // senders (like primsim) who send their EOT packets on 1024-tick
    // boundaries.
    int64_t timestepForTimestamp(int64_t timestamp)
    {
        TSystemTimePoint stpTimestamp{std::chrono::duration_cast<TSteadyTimePoint::duration>(std::chrono::nanoseconds(timestamp))};
        // How many units of TStepTime have passed at `timestamp`?
        size_t nSteps=std::chrono::duration_cast<TStepTime>(stpTimestamp.time_since_epoch()).count();
        // Subtract StepsAtEpoch_ just so that the first timestep we handle is ~0, not ~$BIGNUM
        return nSteps-StepsAtEpoch_;
    }

    // At which index in the StoreM should we store objects for the given timestamp?
    size_t indexForTimestamp(int64_t timestamp)
    {
        return timestepForTimestamp(timestamp) & (StoreMWrap_-1);
    }


// The following are parameters, maybe we move them to a different structure later for tidiness
// They get set in main() before the start and are read only after that.
public:
    // Note that the TNanoTime std::chrono::duration unit is in nanoseconds
    // and       the TStepTime unit is in units of 1024*2MHz ticks (one of JJs blocks in the FPGA)
    int32_t StepStride_;    // The number of TStepTime units to add to go from one step to the next.
    // So a TStepTime d {StepStride_} is a duration of the time in a step.
    // TODO: For now, should be a power of two.
    int32_t StoreMWrap_;    // The total length of the escalator.  Should be a power of two.   So it
    // is a number with only one bit set, so StoreMWrap_ - 1 is a wraparound mask.
    // Same as PLENGTH_ [TODO: Combine these two constants]
    int32_t AlgStride_;     // The window width of data treted by each ALG process in unit of
    // escalator steps.  The ALG can peek further back in the past to get overlaps
    int32_t AlgWidth_;      // Width of window including the overlap time (needed so the space doesn't
    // get reallocated too soon).


    // All these constants are in units of numbers of steps from when it started (i.e. how far up the escalator)
    int32_t EALGSTART_;     // Step that ALG begins to look if it can start
    int32_t ESRTMISSSTART_; // First step SRT usually pushes missing packet reports
    int32_t ESRTMISSEND_;   // One step after step when SRT gives up sending missing packet requests
    // This is the step when ALG know it will see no more
    int32_t EALGEND_;       // Latest step by which the ALG must report it's decision
    // [For now this is the pre-SNB decision, another step or two will
    // eventually be needed for the big rolling window and inter-cavern comms]
    int32_t EASRTDECEND_;   // Step when SRT gices up sending any more trigger decisions, (if the acks
    // are not all back at this point, generate error that someone is dead)
    int32_t EALGSUM_;       // Step when the ALG can finish up, generate the summary record fragment
    // and tell CTL to deallocate all the input buffers for this step.
};

//--------------------------------------------------------------------
// Some code now....
//--------------------------------------------------------------------

// Global instances of the main structures
MemPool gMemPool;
StoreM gStoreM;

//======================================================================
int CTL_Process(unsigned long maxSteps) {
    // Set the thread name for the benefit of gdb
    pthread_setname_np(pthread_self(), "CTL");

    // Loop waiting for the time of each pipleline step starting,
    // inform each other process, look through the storeM to see
    // what needs moving on, then initialise the step for a little ahead.

    // StartTimePoint = Get the current start of step time
    // The CtlCmd and Reply variables are the time of the current step in integer units of
    // TStepTime and may have wrapped because we are only using 32 bits.  So we also
    // have StartNanoTime_ which is the int64_t number of ticks since our Epoch of when
    // the current step began.
    StoreM& m = gStoreM;  // So we can use m.variable everywhere below
    MemPool& mp = gMemPool;

    // How many steps have we taken?
    unsigned int nSteps=0;
    // Some variables for reporting what we've done
    unsigned int nSRTDone=0;
    unsigned int nSRTTimedOut=0;
    unsigned int nALGDone=0;
    unsigned int nALGTimedOut=0;

    m.CtlCmd.v.store(0);

    do {
        // Make a time point from StartNanoTime_ that has had the EPOCH and the local
        // clock correction for this computers clock added, and increment it by one Stride to
        // get the time of the local clock of the next time.  Doing it this way means we do
        // tweak the wait for the next step if the local clock correction changes.
        TStepTime NextTimeFromEpoch {m.StepStride_ * (m.ThisTimeStep_ + 1)};
        TSteadyTimePoint NextTimePoint {m.TimePointSteadyEpoch_ + m.LocalTimeShift_ + NextTimeFromEpoch};
        std::this_thread::sleep_until(NextTimePoint);    // Waits

        if(m.CtlCmd.v.load() == -3){
            TRACE(2, "CTL exiting as CtlCmd=-3");
            break;
        }
        if(m.RecReply.v.load() == -3){
            TRACE(2, "CTL exiting as RecReply=-3");
            m.CtlCmd.v.store(-3);
            break;
        }
        // Increment the step
        if (m.CtlCmd.v.load() != m.ThisTimeStep_){
            TRACE(1, "Error - cross check failed - this is bad, investigate");
        }
        m.ThisTimeStep_++;   // Increment the time step

        int32_t a = m.ThisTimeStep_ & 0x0FFFFFFF;

        // Which entry in StoreM will be used to store this time step?
        int32_t thisStepItemIndex = m.ThisTimeStep_ & (m.StoreMWrap_-1);

        int64_t now_ns=std::chrono::system_clock::now().time_since_epoch().count();
        int64_t stepForNow=m.timestepForTimestamp(now_ns);
        TRACE(3, "CTL step %ld, a=%d, index %d, now %'ld stepNow %ld", m.ThisTimeStep_, a, thisStepItemIndex, now_ns, stepForNow);

        // Wraparound before overflowing a signed 32bit int.  Note it doesn't matter if we
        // wraparound CtlCmd and the Replys at different places, provided both wraps are above
        // the mask we use to compare and the wraps are a power of 2.  Also note the atomic variables
        // are 32bit because on most architectures the compiler might do it lock free. TODO check this

        m.CtlCmdCV.NotifyNewStep(m.CtlCmd,a);    // This locks and does the control variable notify_all() stuff

        // How far can we advance MemPool::FrontPointer_? It will be
        // the minimum distance between the current position and the
        // front of an active StoreMItem (where 'distance' accounts
        // for wraparound). We also mustn't go past IPointer and start
        // freeing things that SRT is still working on
        size_t distToFree=mp.distance(mp.FrontPointer_.load(), mp.IPointer_.load());

        // Now loop through the StoreM and StoreMItem to see what should move on somehow.  REC and SRT can
        // move themselves along, because they watch for every step quickly.  But the ALGs can either sleep
        // or be buy and can delay and we can check on any that are late.
        for (int mn=0; mn<m.PLENGTH_; mn++) {

            StoreMItem& mi = gStoreM.Items_[mn];
            int32_t StepsGone = ((a|m.StoreMWrap_) - mi.StartStep_) & (m.StoreMWrap_-1);
            // StoreMWrap has only one bit set,         e.g. 0x40000000 to undo any wraparound
            // (m.StoreMWrap_ - 1) is needed as a mask, e.g. 0x3FFFFFFF

            if (mi.State_.load() == MItemState::Inactive && (StepsGone > 10)) {  // Stuff hasn't arrived for this step yet.
                // Monitor it somehow
            }

            // Should we start the ALG yet? Yes if all the packets are here, or if time has passed too much.
            // Do this in the ALG processs, not here, because then it can start this step and not wait for the next one

            // PAR 2018-04-06: Giles's code was checking whether SRT was done by looking at mi.NPointer_ like this:
            //
            // if (mi.State_ == MItemState::Initialized && (mi.NPointer_ == mi.NSOURCE_ || StepsGone > 15)) {   // 15, make it a parameter
            //
            // but it seems like mi.SRTstatus has the same information? So use that
            if (mi.State_.load() == MItemState::Initialized){
                const int srtStatus=mi.SRTstatus.load();
                const bool srtDone=srtStatus==StoreMItem::SRTDone;
                const bool timedOut=srtStatus==StoreMItem::SRTTimedOut;

                if(srtDone){
                    ++nSRTDone;
                }

                if(timedOut){
                    ++nSRTTimedOut;
                }

                if(srtDone || timedOut){
                    // Is the item empty? (No packets)
                    if(mi.FirstPointer_.load() == mi.LatestPointer_.load()){
                        mi.State_.store(MItemState::Finished); // Fast-forward to "Finished". No need to try algs on this one
                    }
                    else{
                        mi.State_.store(MItemState::ReadyForAlgs);      // Now in computation.
                    }

                    // TODO The computations are done with a series of
                    // steps to allow a longer stretch of data to be analysed in one go and overlap, so
                    // improve this logic here to wait for multiple steps to be ready and to handle
                    // case where there is no computation for this step.
                    // TODO maybe this logic should be in ALG?
                }
            }

            // Should the ALG have finished yet?  If so, DING it by bumping the State_ so it can output debug info
            if (mi.State_.load() == MItemState::ReadyForAlgs) {
                const bool algDone = mi.ALGFinalDone.load() != 0;
                const bool timedOut = StepsGone > 25;

                if(algDone){
                    ++nALGDone;
                }

                if(timedOut){
                    ++nALGTimedOut;
                }

                if (algDone || timedOut) {  // First condition is normal, second means late
                    mi.State_ = MItemState::AlgsFinished;   // We know it is waiting for the trigger decision to be sent successfully
                }
            }

            if (mi.State_.load() == MItemState::AlgsFinished) {
                if(!mi.FinalDecisions.empty()){
                    TRACE(3, "CTL_Process Trigger: item %d at step %d", mn, a);
                }
                const bool algDone = mi.SRTDecisionFinalSent.load() != 0;
                const bool timedOut = StepsGone > 30;

                if (algDone || timedOut) {
                    mi.State_ = MItemState::Finished;    // This means ALG is finished and CTL can recycle it for a new pipeline step.
                    // TODO Insert a State_=4 which means the triggering has been done, but the info
                    // is still being copied to the SNB central algorithm.
                }
            }

            if(mi.State_.load() == MItemState::Finished){
                mi.State_.store(MItemState::Inactive);
            }

            if(mi.State_.load() == MItemState::Inactive){
                mi.Clear();
                // TODO: SRT reads StartStep_ so it should probably be atomic
                int prevIndex = mn==0 ? (m.PLENGTH_-1) : (mn-1);
                mi.StartStep_ = m.Items_[prevIndex].StartStep_+1;
                TRACE(3, "CTL initializing item %d to startstep %d", mn, mi.StartStep_);
                mi.State_.store(MItemState::Initialized);
            }

            // How far can we move the FrontPointer_? TODO: We really
            // want to just check on items that are !Inactive, but the
            // two steps above make any inactive item immediately move
            // to Initialized, so we have to consider those items
            // "available for reclaim" too
            if(!( mi.State_.load() == MItemState::Inactive || mi.State_.load() == MItemState::Initialized )){
                distToFree = std::min(distToFree, mp.distance(mp.FrontPointer_.load(), mi.FirstPointer_.load()));
            }
        } // end loop over StoreM

        // Actually move the FrontPointer_ the distance allowed
        if(distToFree!=0){
            size_t oldFront=mp.FrontPointer_.load();
            mp.advance(mp.FrontPointer_, distToFree);
            size_t newFront=mp.FrontPointer_.load();
            TRACE(4, "CTL Moved FrontPointer_ by %ld, from %ld to %ld", distToFree, oldFront, newFront);
        }

        if (m.CtlCmd.v.load() == -3) {
            m.CtlCmdCV.NotifyNewStep(m.CtlCmd,-2);  // Locks and tells all the processes to exit
            break;   // Exit the do..while loop which is otherwise infinite
        }
        if(nSteps++==maxSteps){
            m.CtlCmd.v.store(-3);
        }
        if(nSteps==maxSteps+1) break;
    } while (true);

    TRACE(2, "CTL exiting after handling %d steps", nSteps);
    TRACE(2, "             Done       Timed out");
    TRACE(2, "---------------------------------");
    TRACE(2, " SRT   % 10d      % 10d", nSRTDone, nSRTTimedOut);
    TRACE(2, " ALG   % 10d      % 10d", nALGDone, nALGTimedOut);

    return 0;
}

//======================================================================
int REC_Process() {
    // Set the thread name for the benefit of gdb
    pthread_setname_np(pthread_self(), "REC");

    StoreM& m = gStoreM;    // The m.xxxx syntax is convenient below.

    zsock_t* subscriber = zsock_new(ZMQ_DISH);
    zsock_bind(subscriber, "udp://*:5601");
    // Arg is string to subscribe to: blank=everything
    // zsock_set_subscribe(subscriber, "");
    zsock_join(subscriber, "all");

    // Set a timeout so we bail when the sender is done
    zsock_set_rcvtimeo(subscriber, 2000);

    //-----------------------------------------------

    // Conceptually:

    // do {
    //   if (there is room in mempool .AND. a packet has arrived) {
    //     receive the packet and put it into the mempool
    //   }
    //
    //   if (the CtlCmd variable indicates we should exit (by being set to -1) { return 0; }
    // } while (true);

    TSystemTimePoint rec_start=std::chrono::system_clock::now();
    int nPackets=0;
    do {
        int N;
        TriggerPrimitive* ps=recvPrimitives(subscriber, N);
        if(!ps){
            m.RecReply.v.store(-3);
            free(ps);
            break;
        }
        ++nPackets;
        MemPool& mp=gMemPool;

        std::vector<Packet>& packets=mp.pool_;
        const size_t front=mp.FrontPointer_.load();
        for(int i=0; i<N; ++i){
            const size_t pos=mp.BackPointer_.load();

            if(pos+1==front){
                TRACE(0, "Back pointer caught front pointer at %ld", pos);
                m.RecReply.v.store(-3);
                break;
            }

            packets[pos].content_[0] = ps[i].timestamp;
            packets[pos].content_[1] = ps[i].apa;
            packets[pos].content_[2] = ps[i].channel;
            packets[pos].content_[3] = ps[i].time_over_threshold;
            packets[pos].content_[4] = ps[i].charge;
            packets[pos].content_[5] = ps[i].seqno;
            
            // Wraparound
            if(mp.BackPointer_==mp.POOLSIZE_-1) mp.BackPointer_=0;
            else                                mp.BackPointer_++;
        }
        free(ps);
        int stepNo=m.CtlCmd.v.load();
        if(stepNo==-3) break;
    } while(true);
    
    TSystemTimePoint rec_end=std::chrono::system_clock::now();
    long int rec_ms=std::chrono::duration_cast<std::chrono::milliseconds>(rec_end-rec_start).count();
    TRACE(2, "REC_Process exiting. Got %d packets in %ldms, %.1fk pps", nPackets, rec_ms, double(nPackets)/rec_ms);
    
    zsock_destroy(&subscriber);
    return 0;
}

//======================================================================
int SRT_Process() {
    // Set the thread name for the benefit of gdb
    pthread_setname_np(pthread_self(), "SRT");

    StoreM& m = gStoreM;  // So we can use m.variable everywhere below
    MemPool& mp = gMemPool;

    m.SrtReply.v.store(0);

    size_t nEOTsFound=0;

    while(true){
        int currentStep=m.CtlCmd.v.load();
        if(currentStep==-3) break;

        size_t my_ipointer=mp.IPointer_.load();

        // Advance IPointer until we get to an EOT packet, or we reach
        // BackPointer. If we reach BackPointer, wait a while and try
        // advancing further: maybe new packets have arrived. In the
        // meantime, we can check whether any timeout/exit conditions
        // have been set
        size_t my_backpointer=mp.BackPointer_.load();
        while(my_ipointer!=my_backpointer){
            uint64_t timestamp=mp.pool_[my_ipointer].content_[0];
            if(timestamp==0xDEADBEEF){
                TRACE(2, "SRT accessed uninitialized packet at index %ld; backpointer %ld", my_ipointer, my_backpointer);
            }

            size_t stepForStamp=m.timestepForTimestamp(timestamp);
            size_t indexForStamp=stepForStamp & (m.StoreMWrap_ - 1);

            // The timestep item for this packet
            StoreMItem& item=m.Items_[indexForStamp];

            const bool isInitialized=item.State_.load()==MItemState::Initialized;
            if(!isInitialized){
                TRACE(2, "SRT got packet at %ld for item at %ld in state %d!", my_ipointer, indexForStamp, int(item.State_.load()));
            }
            const bool isDone=item.SRTstatus.load()==StoreMItem::SRTDone;
            if(isDone){
                TRACE(2, "SRT already done! at %ld item at %ld, seqno %ld ts %'ld", my_ipointer, indexForStamp, mp.pool_[my_ipointer].content_[5], timestamp);
            }
            const bool isTimedOut=item.SRTstatus.load()==StoreMItem::SRTTimedOut;
            if(isTimedOut){
                //TSystemTimePoint stpTimestamp{std::chrono::duration_cast<TSteadyTimePoint::duration>(std::chrono::nanoseconds(timestamp))};
                TSystemTimePoint stpTimestamp{std::chrono::nanoseconds(timestamp)};
                TSystemTimePoint now=std::chrono::system_clock::now();
                long int us_ago=std::chrono::duration_cast<std::chrono::microseconds>(now-stpTimestamp).count();

                TRACE(2, "SRT got packet at %ld for item at %ld, already timed out! ts %ld, (%ld us ago)", my_ipointer, indexForStamp, timestamp, us_ago);
                m.CtlCmd.v.store(-3);
            }

            // The only items we can do anything with are those that
            // are in the initialized state and not still/already
            // marked as "done" or "timed out"
            const bool valid=isInitialized && (! (isTimedOut || isDone) );

            if(valid){
                // Is the IPointer packet an end-of-timestamp packet?
                const bool is_eot = mp.pool_[my_ipointer].content_[5] == std::numeric_limits<uint32_t>::max();
                uint64_t apa=mp.pool_[my_ipointer].content_[1];

                if(is_eot){
                    // Which source is it the EOT for? Check it's not
                    // a source for which we already have an EOT
                    if(item.NotMissingFlag_[apa]==0){
                        TRACE(4, "SRT valid EOT packet at %ld apa %ld ts %'ld step %ld index %ld", my_ipointer, apa, timestamp, stepForStamp, indexForStamp);
                        assert(apa<StoreMItem::NSOURCE_);
                        item.NotMissingFlag_[apa]=1;
                        item.PacketPointer_[apa]=my_ipointer;
                        ++item.NPointer_;
                        ++nEOTsFound;
                    }
                    else{
                        TRACE(2, "SRT got dupe EOT for source %ld at index %ld", apa, my_ipointer);
                    }
                }// end if(is_eot)
                else{
                    TRACE(4, "SRT valid non-EOT packet at %ld apa %ld ts %'ld step %ld index %ld", my_ipointer, apa, timestamp, stepForStamp, indexForStamp);
                }

                // Add this packet to the list of packets in the relevant item
                if(item.FirstPointer_== StoreMItem::InvalidPtr) item.FirstPointer_=my_ipointer;
                item.LatestPointer_=(my_ipointer+1) & (MemPool::POOLSIZE_ - 1);
            } // end if(valid)

            ++my_ipointer;
            my_ipointer &= (MemPool::POOLSIZE_ - 1);
            mp.IPointer_.store(my_ipointer);
        } // end while(my_ipointer!=my_backpointer){

        // Has this item timed out?
        int32_t a = m.CtlCmd.v.load();

        // Now loop over items and see if any can be marked as done or must be moved on
        for (int itemIndex=0; itemIndex<m.PLENGTH_; ++itemIndex) {
            StoreMItem& item = m.Items_[itemIndex];

            // SRT only deals with steps in the Initialized state, not timed out or done
            const bool isInitialized=item.State_.load()==MItemState::Initialized;
            const bool isDone=item.SRTstatus.load()==StoreMItem::SRTDone;
            const bool isTimedOut=item.SRTstatus.load()==StoreMItem::SRTTimedOut;
            const bool valid=isInitialized && (! (isTimedOut || isDone) );
            if(!valid) continue;

            // Does the item have all the EOTs it needs? Mark it as
            // done, and CTL will handle it from there. Maybe we have
            // to also set State_ here? Seems cleaner to have CTL do it, so it's the only thing that modifies State_
            if(item.NPointer_==StoreMItem::NSOURCE_){
                TRACE(3, "SRT item %d got all %d EOTs. Marking as done (%ld, %ld)",
                      itemIndex, item.NPointer_, item.FirstPointer_.load(), item.LatestPointer_.load());
                item.SRTstatus.store(StoreMItem::SRTDone);
            }

            // TODO: StartStep_ probably needs to be atomic
            int32_t StepsGone = ((a|m.StoreMWrap_) - item.StartStep_) & (m.StoreMWrap_-1);
            if(a>item.StartStep_ && StepsGone>15 && item.SRTstatus.load()!=StoreMItem::SRTTimedOut){
                TRACE(3, "SRT item %d timed out: start %d, now %d", itemIndex, item.StartStep_, a);
                item.SRTstatus.store(StoreMItem::SRTTimedOut);
            }
        } // end loop over items

        // Has the control command been set to stop?
        if(m.CtlCmd.v.load()==-3) break;

        // Wait a little bit for more packets to come in
        // std::this_thread::sleep_for(std::chrono::microseconds(2));

    } // end while(true) loop

    TRACE(2, "SRT_Process exiting. Found %ld EOTs", nEOTsFound);

    return 0;
}

//======================================================================
int ALG_Process(int alg_index) {    // an integer for the ALG process number (0 to PLENGTH_-1)
                            // It can be used in the ALG process to access the correct StoreMItem

    // Set the thread name for the benefit of gdb
    std::string algname("ALG");
    algname+=std::to_string(alg_index);
    pthread_setname_np(pthread_self(), algname.c_str());

    StoreM& m = gStoreM;    // The m.xxxx syntax is convenient below.
    // MemPool& mp = gMemPool;
    // Wait for the next step
    StoreMItem& myItem=m.Items_[alg_index];
    while(true){
        while(!m.CtlCmdCV.WaitNextStepFromCTL(m.CtlCmd, myItem.AlgReply)){}
        int stepNo=m.CtlCmd.v.load();
        if(stepNo==-3) break;
        myItem.AlgReply.v.store(stepNo);

        // Is my item ready to be ALGed? TODO: Replace this with
        // something better, like a wait on a condition variable
        if(myItem.State_==MItemState::ReadyForAlgs){
            // We want a sorted list of channels of primitives in this step, so make that
            size_t begin=myItem.FirstPointer_;
            size_t end=myItem.LatestPointer_;
            // This step is empty: nothing to do
            if(begin==end) continue;

            // // Handle wraparound
            // size_t nPrims=end>begin ? (end-begin) : ((mp.POOLSIZE_ - begin) + end);
            // std::vector<uint64_t> primChannels(nPrims);
            // for(size_t i=0; i<nPrims; ++i){
            //     primChannels[i]=mp.pool_[(begin+i) & (mp.POOLSIZE_ - 1)].content_[2];
            // }
            // std::sort(primChannels.begin(), primChannels.end());

            // unsigned int nClusters=0;
            // unsigned int i=0;
            // unsigned int N=3, M=5; // Require 3 hits in 5 channels to form a cluster
            // // If there aren't at least N primitives, there can't be any clusters
            // if(nPrims>=N && primChannels.size()>M){
            //     while(i<primChannels.size()-M){
            //         if(m.CtlCmd.v.load()==-3) break;
            //         uint64_t thisChannel=primChannels[i];
            //         // How far in channels do we have to go to get N hits?
            //         uint64_t nthChannel=primChannels[i+N-1];
            //         int64_t channelDiff=nthChannel-thisChannel;
            //         if(channelDiff<M-1){
            //             // We've got a cluster. Count it and advance i to the end of the cluster
            //             ++nClusters;
            //             // TODO: Do this properly
            //             i+=N;
            //         }
            //         else{
            //             ++i;
            //         }

            //     } // end cluster-finding loop
            // } // end if (nPrims>=N)
            // if(nClusters>0){
            //     TRACE(3, "ALG_Process %d found %d clusters", alg_index, nClusters);
            //     myItem.FinalDecisions.push_back(TrigDecision{});
            // }

            myItem.ALGFinalDone.store(1);

            // Not quite sure what this is for. Presumably it should
            // really be set elsewhere, but it gets checked in CTL to
            // decide whether to move this step along, so set it here
            myItem.SRTDecisionFinalSent.store(1);
        } // end if (myItem.State_==MItemState::ReadyForAlgs)

        if(m.CtlCmd.v.load()==-3) break;
    } // end while(true)

    return 0;
}

//======================================================================
int main(int argc, char** argv) {
    setlocale(LC_NUMERIC, ""); // So we can get thousands separators in timestamps

    // Initialise all the structures to zero before the other threads start
    TNanoTime NanoStride {1024000};  // This pipeline size is two of JJs RCE 1024tick blocks in nanosecs
    TStepTime StepStride {2};        // This is the same time interval, 2 of JJs 1024 tick blocks, now
                                     // in units where the value is '2'

    TNanoTime LocalClockShift {0};   // Can set a different value if we decide our clock is a bit slow or fast
    std::chrono::time_point<std::chrono::steady_clock> FarmEpoch {};   // This is 0, the standard Unix Epoch I think

    // Initialise the 'configuration constants' which are in StoreM.
    StoreM& m = gStoreM;    // The m.xxxx syntax is convenient below.
    // Note that the TNanaoTime std::chrono::duration unit is in nanoseconds
    // and       the TStepTime unit is in units of 1024*2MHz ticks (one of JJs blocks in the FPGA)
    m.StepStride_=1;  // [int] #TStepTime units to add to go from one step to the next.
    // So 'TStepTime d {StepStride_}' is a duration of the time in a step.
    // TODO: For now, should be a power of two.
    m.StoreMWrap_=m.PLENGTH_; // [int]The total length of the escalator.  Should be a power of two.  So it
    // is a number with only one bit set, so StoreMWrap_ - 1 is a wraparound mask.
    m.AlgStride_=4;   // The window width of data treated by each ALG process in unit of
    // escalator steps.  The ALG can peek further back in the past to get overlaps
    m.AlgWidth_=5;    // Width of window including the overlap time (needed so the space doesn't
    // get reallocated too soon).


    // All these constants are in units of numbers of steps from when it started (i.e. how far up the escalator)
    m.EALGSTART_=5;     // Step that ALG begins to look if it can start
    m.ESRTMISSSTART_=7; // First step SRT usually pushes missing packet reports
    m.ESRTMISSEND_=15;  // One step after step when SRT gives up sending missing packet requests
    // This is the step when ALG know it will see no more
    m.EALGEND_=22;      // Latest step by which the ALG must report it's decision
    // [For now this is the pre-SNB decision, another step or two will
    // eventually be needed for the big rolling window and inter-cavern comms]
    m.EASRTDECEND_=30;  // Step when SRT gices up sending any more trigger decisions, (if the acks
    // are not all back at this point, generate error that someone is dead)
    m.EALGSUM_=24;      // Step when the ALG can finish up, generate the summary record fragment
    // and tell CTL to deallocate all the input buffers for this step.


    // Preset the first few (=8) steps to activate, starting say 10 steps from
    // now; and the control-reply system to activate then as well.
    // TODO: Think hard about rollover of variables [Use Linux Kernel 'jiffies trick' of
    // setting epoch so rollover happens in 5 minutes time and we can test it regularly.

    // TODO: These are not _quite_ the same time because of the time
    // taken to do the function calls, but I don't see a way to find
    // one time and convert. This will have to do for now
    m.TimePointSystemEpoch_=std::chrono::system_clock::now();   // Time when the program started by the system clock
    m.TimePointSteadyEpoch_=std::chrono::steady_clock::now();   // Time corresponding to TimePointSystemEpoch in the steady clock

    // Align our epoch to the previous tick of TStepTime in the overall system epoch
    m.StepsAtEpoch_=std::chrono::duration_cast<TStepTime>(m.TimePointSystemEpoch_.time_since_epoch()).count();

    auto tp1 = std::chrono::steady_clock::now();
    TNanoTime NanoTime10FromNow = (tp1 - LocalClockShift - FarmEpoch + 10*NanoStride);
    TStepTime StepTime10FromNow = std::chrono::duration_cast<TStepTime>(NanoTime10FromNow);
    int Step10FromNow = StepTime10FromNow.count();

    m.CtlCmd.v.store(Step10FromNow);
    m.RecReply.v.store(Step10FromNow);
    // m.SrtReply.v.store(Step10FromNow);
    m.CtlReply.v.store(Step10FromNow);

    // "Parse" command line arguments
    unsigned long maxSteps=atol(argv[1]);
    StoreMItem::NSOURCE_ = atoi(argv[2]);
    TRACE(2, "trigfarm main: running for %ld steps (%ld ms) with %d sources",
          maxSteps, std::chrono::duration_cast<std::chrono::milliseconds>(double(maxSteps)*TStepTime{1}).count(),
          StoreMItem::NSOURCE_);

    // Start all the other threads
    std::thread ctl_thread {CTL_Process, maxSteps};
    std::thread rec_thread {REC_Process};
    std::thread srt_thread {SRT_Process};
    for (int mi=0; mi < gStoreM.PLENGTH_; mi++) {
        // for (auto m = gStoreM.store_.begin(); m<gStoreM.store_.end(); mi++) {
        // for (auto m : gStoreM.store_) {
        std::thread tmp {ALG_Process,mi};
        gStoreM.Items_[mi].AlgThread = std::move(tmp);
    }

    // Wait for the threads to shut down.  This should only happen if CTL sets CtlCmd in storeM to some magic cookie value
    ctl_thread.join();
    rec_thread.join();
    srt_thread.join();
    for (int mi=0; mi < gStoreM.PLENGTH_; mi++) {
        gStoreM.Items_[mi].AlgThread.join();
    }

    auto tp2 = std::chrono::steady_clock::now();
    TRACE(2, "trigfarm main: all shut down");
    TRACE(2, "trigfarm main: ran for %ld steps: %ld ms sim time, %ld ms walltime\n",
          maxSteps,
          std::chrono::duration_cast<std::chrono::milliseconds>(double(maxSteps)*TStepTime{1}).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(tp2-tp1).count()
          );

    return 0;   // Everything shut down nicely
}

// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:
