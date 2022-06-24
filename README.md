# http-proxy

## build
```
git clone https://github.com/fantasy-peak/http-proxy.git
cd http-proxy && mkdir build && cd build
cmake ..
make -j8
```
## start
```
./http-proxy --help
Flags from /home/fantasy/http-proxy/src/main.cpp:
    -host (http proxy host) type: string default: "0.0.0.0"
    -log (log path) type: string default: "./http-proxy.log"
    -log_level (default info level) type: int32 default: 1
    -open_file_log (open file log) type: bool default: false
    -port (http proxy port) type: int32 default: 8849
    -threads (work threads) type: int32 default: 8
    -timeout (connect timeout ms) type: int32 default: 5000

// log_level=0 mean open debug log, log_level=1 mean open info log
./http-proxy --host=0.0.0.0 --port=8850 --log_level=0
```