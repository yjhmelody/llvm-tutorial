export CPLUS_INCLUDE_PATH=/usr/include/llvm-6.0:$CPLUS_INCLUDE_PATH

# clang++ -g -O3 ./src/main.cpp `llvm-config --cxxflags` -std=c++11
clang++ -g -O3 ./src/main.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o main.out
