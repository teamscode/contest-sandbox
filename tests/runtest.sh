#! /bin/bash
set -ex
dir=$PWD
python3 -V
gcc -v
g++ -v

cd $dir
rm -rf build && mkdir build && cd build && cmake ..
make
make install
cd ../bindings/Python && rm -rf build
python3 setup.py install
cd ../../tests/Python_and_core && python3 test.py
