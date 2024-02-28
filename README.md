## Note
**To reduce the size of repository, we removed unused modules/files from ns-3.**

**Following directories and files are removed:**
- `.github/`
- `contrib/`
- `doc/`
- `examples/`
- `utils/`
- `src/*/{doc,examples,test}`
- `src/{antenna,aodv,brite,buildings,click,config-store,csma,csma-layout,dsdv,dsr,energy,fd-net-device,lr-wpan,lte,mesh,mobility,mpi,netanim,nix-vector-routing,olsr,openflow,point-to-point-layout,propagation,sixlowpan,spectrum,tap-bridge,test,topology-read,uan,virtual-net-device,visualizer,wifi,wimax}`
- `.gitattributes`
- `.mailmap`
- `CHANGES.md`
- `CONTRIBUTING.md`
- `RELEASE_NOTES.md`
- `pyproject.toml`
- `setup.cfg`
- `setup.py`
- `test.py`
- `utils.py`

> If you need to use any of the above modules in `src/` directory, you can manually copy the module you need and all its dependencies back to the `src/` directory.
You can find the dependencies of a module by checking `LIBRARIES_TO_LINK` arguments in `<module>/CMakeLists.txt`.
> 
> For example, to find the dependencies of `wifi` module, we first go to `src/wifi/CMakeLists.txt` and find out that `LIBRARIES_TO_LINK` contains `libenergy` and `libspectrum`, which means module `wifi` depends on `energy` and `spectrum`. Similarly, we can find that module `energy` depends on `network` (already there), module `spectrum` depends on `propagation` and `antenna`, module `propagation` depends on `mobility`. Therefore, to use `wifi` module, you need to copy `src/{wifi,energy,spectrum,propagation,antenna,mobility}` back to `src/` directory.

<br>


**A line in `src/network/CMakeLists.txt` is deleted:**
```diff
-     test/header-serialization-test.h
```

<br>

**Modifications made to the top level `CMakeLists.txt`:**
- As `examples/` and `utils/` are removed, corresponding `add_subdirectory()` statements are commented out.
  ```cmake
  # Build NS library examples
  # add_subdirectory(examples)

  # Build scratch/simulation scripts
  add_subdirectory(scratch)

  # Build test utils
  # add_subdirectory(utils)
  ```

- To prevent "uninitialized variable" warning when executing `./ns3 configure` (caused by the removal of `brite, click, fdnetdev, openflow`), related variables are initialized to `OFF` at the beginning.
  ```cmake
  set(NS3_BRITE    OFF)
  set(NS3_CLICK    OFF)
  set(NS3_OPENFLOW OFF)
  set(ENABLE_EMUNETDEV  OFF)
  set(ENABLE_FDNETDEV   OFF)
  set(ENABLE_NETMAP_EMU OFF)
  set(ENABLE_TAPNETDEV  OFF)
  ```

<br>
<br>

To speed up cmake reconfigure (when there is source file added/removed),
`find_package/check_deps` for boost, gsl, doxygen and sphinx are skiped (Related statements are commented out in `build-support/macros-and-definitions.cmake`).