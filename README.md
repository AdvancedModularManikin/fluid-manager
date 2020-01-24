# AMM - Fluid Manager

#### Requirements
The Fluid Manager requires the [AMM Standard Library](https://github.com/AdvancedModularManikin/amm-library) be built and available (and so requires FastRTPS and FastCDR).  In addition to the AMM library dependancies, the Fluid Manager also requires:
- tinyxml2 (`apt-get install libtinyxml2-dev`)
- [SPI Remote Library](https://github.com/peterohanley/spi_proto) (included as submodule)
 

### Installation
```bash
    $ git clone https://github.com/AdvancedModularManikin/fluid-manager
    $ cd fluid-manager
    $ git submodule update --init
    $ mkdir build && cd build
    $ cmake ..
    $ cmake --build . --target install
```

By default on a Linux system this will install into `/usr/local/bin`
