#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>

// Currently this class has only one variable, so is a bit superfluous, it contains
// the atomic that is used for the command or reply between the processes as the pipeline steps occur.
class TFarmCVV {
public:
    std::atomic<int> v;  // The command or reply variable in the inter-thread communication on one trigger farm node.
};

//-------------------------------------------------------------------------
// Small class to hide the details of the condition_variable and the mutex
// These have been put together in a standardised way using the examples
// in the Bjarne Stroustrup book.
//-------------------------------------------------------------------------
class TFarmCV {
public:
    // Constructor
    TFarmCV(int32_t mask = 0xffffffff) : mask_(mask) {}

    // NotifyNewStep:
    // CTL calls this when it has been woken up at the start of a new pipeline 
    // step. t is the index of the pipeline step that is just beginning.  It should
    // be different from all the Reply variables (usually a little greater than, 
    // except at wraparound) so that the condition variable condition knows when 
    // to wakeup.  To make wraparound easy, assuming that the escalator contains a
    // number of steps that is a power of two, the comparison applies the mask 
    // and then compares the control variable for inequality with the reply variable.
    // This allows the controller and receiver to go a bit above the wraparoud
    // point and wrap at a convenient point a bit beyond, without any concurrency
    // difficulties.
    void NotifyNewStep(TFarmCVV& cmd, int32_t stepNumber) {
        // This is essentially coppied from Stroustrup p1232
        std::lock_guard<std::mutex> lck(mtx_);    
        // Note Stroustrup uses lock_guard instead of unique_lock
        // It is slightly less complicated.
        cmd.v.store(stepNumber);        // This make the condition that things should wakeup
        cond_.notify_all();             // Unblock all waiting processes, they will acquire the lock in turn
        // and do their thing (which is in the function below).
        // cond_.notify_all() implicitly releases the lock, 
        // but the lock_guard ensures it is released when we leave.
    }

    // PollNextStepFromCTL():
    // SRT and perhaps ALG and others call to ask whether the pipeline step has 
    // changed.  This is useful when the thread knows it still has stuff to do and
    // doesn't want to wait.  If the pipeline step has changed, returns bool.
    // The process must take care of acknowledging to CTL, by incrementing the
    // return variable. 
    bool PollNextStepFromCTL(TFarmCVV& cmd, TFarmCVV& reply) { 
        return (cmd.v.load() & mask_) != (reply.v.load() & mask_); 
    }

    // StepReply():
    // Increments the Reply variable to acknowledge to CTL that a step has been taken.
    // Note it is better to increment rather than set equal to CtlCmd (as that could
    // step by more than one if the next step is taken while this is beong called).
    // StepReply does not check that it is not beong premature, i.e. the caller
    // should only call it once either PollNextStepFromCTL() or WaitNextStepFromTL() have
    // returned true.
    void StepReply(TFarmCVV& reply) { 
        int32_t a = ++(reply.v);   //reply.v is atomic
        // a is a local (non-atomic) copy of new value  
        if (a>2*(mask_+1)) {   // We are a suitable distance above rollover
            a &= mask_;   // Mask out bits above mask_ and store back into atomic
            // Since all processes use mask_ to compare they will not
            // break if somehow reading the intermediate value
            reply.v.store(a);  // We could have written reply.v = a instead.
        }
    }

    // Note the warning in Barne Stroustrup's book page 1231.  "When a condition 
    // variable is destroyed, all waiting threads (if any) must be notified (i.e. 
    // told to wake up) or they may wait forever".

    // WaitNextStepFromCTL():
    // Similar to PollNextStepFromCTL() except that if the condition is not set
    // yet, it will sleep a while.  This is where we can hide away experimenting with
    // using a real condition variable or a this_thread::sleep().  Note that it
    // can return 'spuriously' with a false which means the caller should wait a bit
    // more.   Eventually we will ave to handle the case that the CTL has died in
    // which case we try not to wait forever.
    bool WaitNextStepFromCTL(TFarmCVV& cmd, TFarmCVV& reply,int32_t t=500000) {
        // This is essentially copied from Stroustrup p1233
        std::unique_lock<std::mutex> lck(mtx_);  
        // This ensures lock is released on exit
        // Stroustrup says lock_guard is not good enough here
        // because unique_lock can handle that cond_.wait_for()
        // unlocks and then locks again.
        std::chrono::duration<uint64_t,std::nano> delta_t {t};  // 500us default
        bool gotit = cond_.wait_for(
            lck,    // The lock is released, we wait, and then it is reatained
            delta_t,
            [&cmd,&reply,this]{return (cmd.v.load() & mask_) != (reply.v.load() & mask_); });   
        // Lambda function.  The [] is the capture list and specifies cmd and reply are 
        // accessed by reference and [this] makes sure mask_ is captured for use by the lambda
        return gotit;   // true=step incremnted, false=timed out or spurious wakeup
        // We now release mtx_ again here.  
    }

private:
    int32_t mask_;   // Mask when comparing for equality to aid wraparound
    // Probably this is an overkill and not needed(?)
    // To use, 

    std::mutex mtx_;
    std::condition_variable cond_;
};

// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:


