# third_party/libmicrohttpd — OPTIONAL vendored microhttpd

Default behaviour: rely on the system apt package `libmicrohttpd-dev`.
Vendoring only matters on air-gapped machines / Jetson where you can't `apt`.

## Enable

```bash
./download.sh
cmake -B build -DBUILD_VENDORED_LIBMICROHTTPD=ON ...
```

## Cross-compile

```bash
LIBMICROHTTPD_VERSION=1.0.1 ./download.sh
cmake -S third_party/libmicrohttpd -B third_party/libmicrohttpd/build \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DBUILD_VENDORED_LIBMICROHTTPD=ON \
    -DCMAKE_INSTALL_PREFIX=$PWD/install/vendored
cmake --build third_party/libmicrohttpd/build -j
```