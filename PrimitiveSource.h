#ifndef PRIMITIVE_SOURCE_H
#define PRIMITIVE_SOURCE_H

#include <fstream>
#include <sstream>
#include <vector>
#include <memory>

#include "TRandom.h"

#include "TriggerPrimitive.h"

// Number of collection wires per APA. Is this right?
constexpr int COLL_PER_APA=480;
// Sampling rate in Hz
constexpr float SAMPLING_RATE=2e6;

//======================================================================
namespace{
    // Throw a Poisson variate with the given value of exp(-mean), saving
    // us an exp() call. Otherwise copied from TRandom::Poisson()
    int fastPoisson(double expmean)
    {
        double pir = 1;
        int n = -1;
        while(true) {
            n++;
            pir *= gRandom->Rndm();
            if (pir <= expmean) return n;
        }
    }
}

//======================================================================
class PrimitiveSource
{
public:
    // Generate a set of primitives in times between timestamp and
    // timestamp+windows, and add them to prims
    virtual void addPrimitives(uint64_t timestamp, int window,
                               std::vector<std::shared_ptr<TriggerPrimitive> >& prims) const = 0;
};

//======================================================================
class NoiseSource : public PrimitiveSource
{
public:
    NoiseSource(float rate, float mean, float sigma)
        : m_rate(rate),
          m_mean(mean),
          m_sigma(sigma),
          m_expRate(exp(-m_rate))
    {}

    virtual void addPrimitives(uint64_t timestamp, int window,
                               std::vector<std::shared_ptr<TriggerPrimitive> >& prims) const override
    {
        // We'll assume all the noise hits are three ticks long,
        // so by the time we read them out, they started three
        // ticks ago. So no hits before three ticks
        if(timestamp<3) return;

        int nHits=fastPoisson(m_expRate);
        for(int i=0; i<nHits; ++i){
            // Pick a random channel for the primitive, uniformly
            int channel=gRandom->Integer(COLL_PER_APA);
            // Pick a random time in the window
            int time=timestamp+gRandom->Integer(window);
            // Looks like noise hits have a Gaussian distribution of summed-ADC. Need to pick the +ve side
            int charge=-1;
            while(charge<20){
                // TODO: Could be more efficient somehow by using numbers that are -ve too
                charge=gRandom->Gaus(m_mean, m_sigma);
            }
            auto prim=std::make_shared<TriggerPrimitive>(time,
                                                         0,
                                                         channel,
                                                         3,
                                                         charge);
            prims.emplace_back(std::move(prim));
        }
    }

protected:
    float m_rate;
    float m_mean;
    float m_sigma;
    float m_expRate; // exp(-m_rate)
};

//======================================================================
class LibrarySource : public PrimitiveSource
{
public:
    LibrarySource(float rate, const char* infile)
        : m_rate(rate),
          m_expRate(exp(-m_rate)),
          m_currentEvt(0)
    {
        std::ifstream fin(infile);

        std::string line;
        while(getline(fin, line)){
            m_library.push_back({});
            std::istringstream iss(line);

            // Each line begins with the number of hits on the line, followed by "/"
            std::string delim;
            unsigned int nHits;
            iss >> nHits >> delim;
            assert(delim=="/");

            // Then the hits follow: "time chan sadc time_over_threshold /" for each
            int time, chan, sadc, size;
            unsigned int nRead=0;
            while(iss >> time >> chan >> sadc >> size >> delim){
                m_library.back().emplace_back(std::make_shared<TriggerPrimitive>(time, 0, chan, size, sadc));
                ++nRead;
            }
            assert(nRead==nHits);
        }
    }

    unsigned int size() { return m_library.size(); }
    
    const std::vector<std::shared_ptr<TriggerPrimitive> >& getEvent(unsigned int i) const
    {
        return m_library.at(i);
    }

    virtual void addPrimitives(uint64_t timestamp, int window,
                               std::vector<std::shared_ptr<TriggerPrimitive> >& prims) const override
    {
        // Should I generate an event at this timestep?
        bool genEvent=gRandom->Rndm() < (1-m_expRate);
        if(!genEvent) return;

        // Get the next event in the library
        auto const& evt=getEvent(m_currentEvt);
        // Pick a random channel offset for the primitives, uniformly
        int channel=gRandom->Integer(COLL_PER_APA);
        // Pick a random time in the window
        int time=timestamp+gRandom->Integer(window);
        for(auto const& origPrim: evt){

            auto newPrim=std::make_shared<TriggerPrimitive>(time+origPrim->timestamp,
                                                            0,
                                                            channel+origPrim->channel,
                                                            origPrim->time_over_threshold,
                                                            origPrim->charge);
            prims.emplace_back(std::move(newPrim));
        }
        // Advance the current event, going back to the beginning if
        // necessary
        if(m_currentEvt<m_library.size()-1){
            m_currentEvt++;
        }
        else{
            m_currentEvt=0;
        }
    }

protected:
    float m_rate;
    float m_expRate;
    mutable std::size_t m_currentEvt;
    std::vector<std::vector<std::shared_ptr<TriggerPrimitive> > > m_library;
    
};

#endif

// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:
