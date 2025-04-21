
# Code\_Saturne / LUMA coupling simulation

This repository contains material related to running Code\_Saturne
coupled to LUMA:

<!-- [Build scripts](bin/build-all) - Scripts to install Code\_Saturne and LUMA from the coupling branches.-->
- [Tutorial](tutorial.md) - An end-user tutorial for running a coupled Code\_Saturne
  / LUMA simulation to simulate a lid-driven cavity flow with LUMA and
  Code_Saturne evolving the left and right halves of the domain
  respectively.
<!--- [Example case](cases/ldc_left_right) - The case definition files for
  the lid-driven cavity case above-->
- [Components](components) - Installation packages of Code\_Saturne and LUMA.

## Building

To build the coupled codes, first clone this repository, either using ssh
```
git clone --recursive https://github.com/yangzhou-10/code_saturne-LUMA-coupling.git
```
or https
```
git clone --recursive https://github.com/yangzhou-10/code_saturne-LUMA-coupling.git
```

The `--recursive` flag is needed to ensure that the Code\_Saturne and
LUMA submodules are also cloned.

The LUMA installation package is located in
```
components/LUMA 
```
The process of installation is shown in
```
https://github.com/cfdemons/LUMA/wiki/Installing-LUMA
```
The parallel location exchange (PLE) coupling library is included as a part of code\_saturne CFD tool. The installation package of code\_saturne is located in the file
```
components/code_saturne
```
The process of installation is shown in
```
https://www.code-saturne.org/documentation/7.1/doxygen/src/md__i_n_s_t_a_l_l.html
```

The software should be installed in the location you specified.  This
location can then be used when following the [Tutorial](tutorial.md),
rather than the location specified there.

<!--By default, build-all installs in "development" mode, which means that
no version numbers are appended to the destination directories.  To
see the paths that would be used in release mode, use
```
bin/build-all --mode release --dry-run DESTDIR
```
and if they are OK, perform the installation with
```
bin/build-all --mode release DESTDIR
```

The codes can then be used, for example according to the
[Tutorial](tutorial.md), by setting up the environment with

```
module purge
source DESTDIR/setup.sh
```
-->
