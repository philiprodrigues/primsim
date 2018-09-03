# primsim
Tools for simulations and calculations with DUNE FD trigger primitives.

# Building

Depends on the following:
* ROOT: http://root.cern.ch
* zeromq: http://zeromq.org
* czmq: http://czmq.zeromq.org
* TRACE: https://cdcvs.fnal.gov/redmine/projects/trace/wiki

Make sure that `root-config` is in `$PATH` and that `$PKG_CONFIG_PATH`
and `$LD_LIBRARY_PATH` point to the appropriate installation
directories of zeromq and czmq.

On a system with access to `cvmfs`, appropriate versions of `ROOT` and `TRACE` can be set up via:

```
source /cvmfs/fermilab.opensciencegrid.org/products/larsoft/setups
setup root v6_12_04e -q e15:prof
setup TRACE v3_13_04
```

Once that's all done, a simple `make` should build everything

# Running

The two main executables are `primsim`, which simulates trigger
primitives and sends them out on a zeromq socket, and `trigfarm01`,
which receives trigger primitives from one or more `primsim` instances
and stores them using the escalator protocol.