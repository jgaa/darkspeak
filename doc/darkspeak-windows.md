# Building under Windows

I am using 64 bit Windows 10 with Visual studio 14 (2015).

- Get Microsoft Visual Studio 14 (2015) or newer (I'm using the free community version)
- Get and compile boost (www.bost.org) for 64 bits
- Get cmake
- Get QT Studio for 64 bit Windows (I am using QT 5.6)
- Get the source code from github, initialize the dependencies):

```sh
$ git clone https://github.com/jgaa/darkspeak.git
$ cd darkspeak
$ git submodule init
$ git submodule update
```
- Start Cmake (I use the Cmake GUI under Windows)
In Cmake, add the following paths (I am showing the paths on my development machine):
```
BOOST_ROOT path C:/devel/boost_1_60_0
BOOST_LIBRARYDIR path C:/devel/boost_1_60_0/stage/lib/x64
CMAKE_PREFIX_PATH path C:/Qt/Qt5.6.0/5.6/msvc2015_64
```
- Configure and generate in Cmake
- Find darkspeak.sln (it will be in darkspeak/build on my system) and double click on it to start Visual Studio
- In Visual Studio, build the solution.
