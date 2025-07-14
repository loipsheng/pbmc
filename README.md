# PBMC

This repository contains the prototype research code for PBMC, a pseudo-boolean model counter. 

PBMC is built on top of GPMC and RoundingSat. 

## Setting up

Please ensure that `git` and `cmake` are installed on the system and that the `git clone` command works.

This repository contains the partial code required to build the PBMC model counter. In particular, the `pbmc-mod` folder contains files that were modified from the GPMC model counter and `roundingsat-mod` folder contains files that were modified from the roundingsat PB solver.

To get all the remaining files in place to build PBMC, please run the `setup-complete-code.sh` script or manually follow the steps in the script. It will clone from the GPMC and RoundingSat repositories, checkout to the required commits and put the files in place for compilation.

Run `clean-up.sh` to remove the downloaded files and directories created by `setup-complete-code.sh`.

## Compiling PBMC

Please only proceed once the previous section has been completed i.e. all required source files are cloned and moved into place using the `setup-complete-code.sh` script.

To compile PBMC please run  (a) `./build.sh r` to compile a dynamically linked binary or (b) `./build.sh rs` to compile a statically linked binary of PBMC.

Run `./clean.sh` to remove the compilation files and binaries.

## Running PBMC

To use PBMC to count the number of model for a PB formula specifed in an input.opb file, run PBMC as follows:
```
./pbmc <input.opb>
```

## Input format

PBMC expects input format to follow the .opb format, an example is as follows:
```
* #variable= 3 #constraint= 2
*
+2 x1 -3 x2 +4 ~x3 >= 4;
+1 x1 +1 x2 = 1;
```
 Here `~x1` refers to the literal `not x1`. The above example has 2 pb constraints, with a total of 3 variables. Variables are to be in numerical order, meaning `#variable= 3` should mean that only `x1`, `x2`, and `x3` (and their negated literals) appear in the file.

PBMC has prototype support for projected PB model counting instances. Specify the projection set variables using the following:
```
* #variable= 3 #constraint= 2
* p show 3 0
*
+2 x1 +3 x2 +4 ~x3 >= 4;
+1 x1 +1 x2 = 1;
```
To indicate that the projection set consists of one variabl, `x3`.

To run PBMC on projected model counting use:
```
./pbmc -mode=2 <input.opb>
```

PBMC takes only single line projection set specifications i.e. specifying projection set over multiple `* p show` lines will not work.

## License

Please refer to `LICENSE` file for license of files in this repository.