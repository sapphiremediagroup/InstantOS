rm -rf build
cmake -S . -B build
cmake --build build --target iso --parallel 4
make -C build iso -j4
./run.sh