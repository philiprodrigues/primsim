#include <cstdio>
#include <iostream>
#include <map>

#include "util/tree/ChainWrapper.h"
#include "util/plot/myPlotStyle.h"

enum PType{ kUnknown, kMarl, kAPA, kCPA, kAr39, kNeut, kKryp, kPlon, kRdon, kAr42, kNPTypes};
const char* types[kNPTypes]={"Noise", "Marley", "APA", "CPA", "Ar39", "Neutrino", "Kr85", "Po210", "Rn222", "Ar42"};

void calc_rates(int maxEntries=-1)
{
    myPlotStyle();

    ChainWrapper chw("DAQSimTree");
    chw.Add("/data/lar/dunedaq/MC/sn_hits_from_alex/FH15_430k.root");
    
    const double sampling_rate=2e6; // 2MHz sampling rate
    const int nSamples=4492; // number of ticks per readout window
    int nEntries=maxEntries>0 ? std::min(chw.GetEntries(), maxEntries) : chw.GetEntries(); // number of readout windows
    const int nAPAsFull=150; // number of APAs in a full 10kt module
    const int nAPAsSim=12; // number of APAs actually simulated

    std::map<int, int> hitsByType;
    for(int i=0; i<kNPTypes; ++i){
        hitsByType[i]=0;
    }

    for(int i=0; i<nEntries; ++i){
        if(i%1000==0){
            std::cout << (i/1000) << "k " << std::flush;
        }
        int nHits=chw.GetInt("NColHits", i);
        for(int j=0; j<nHits; ++j){
            int ptype=chw.GetInt("GenType", i, j);
            hitsByType[ptype]++;
        }
    }
    std::cout << std::endl;

    // Make the reverse map so we can sort the output by rate
    std::map<int, int> hitsByType_rev;
    for(int i=0; i<kNPTypes; ++i){
        hitsByType_rev[ hitsByType[i] ] = i;
    }
    printf("                    N     ModRate    APARate APARate/tick\n");
    printf("----------------------------------------------------------\n");
    for(auto it=hitsByType_rev.rbegin(); it!=hitsByType_rev.rend(); ++it){
        int nHits=it->first;
        int ptype=it->second;
        // The number of seconds we've simulated
        float seconds=nSamples*nEntries/sampling_rate;
        // The rate per 10kt module
        float moduleRate=nHits/seconds * (nAPAsFull/nAPAsSim);
        float apaRate=nHits/seconds /nAPAsSim;
        float tickRate=apaRate/sampling_rate;
        printf("%10s % 10d    %.2e   %.2e     %.2e\n", types[ptype], nHits, moduleRate, apaRate, tickRate);
    }

}


// Local Variables:
// mode: c++
// c-basic-offset: 4
// c-file-style: "linux"
// End:
