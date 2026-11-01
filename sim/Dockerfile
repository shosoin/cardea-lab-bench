# SE05x simulator for lab use: the TCP server of wolfSSL's SE050Sim
# (https://github.com/wolfSSL/simulators, GPL-3.0-or-later), pinned to a commit, plus our patches.
# It speaks raw T=1-over-I2C frames on TCP port 8050. NOT a security device.
FROM rust:1.85-bookworm AS build
ARG SIMULATORS_COMMIT=3f557793727685dc70a9295576e29ce3ebfe8e9c
WORKDIR /src
RUN git clone https://github.com/wolfSSL/simulators.git . && git checkout --quiet "$SIMULATORS_COMMIT"
COPY patches/ /patches/
RUN for p in /patches/*.patch; do git apply "$p"; done
# The dev-dependency (the Rust driver used by its tests) is not needed for the server.
RUN cd SE050Sim/se050-sim && sed -i '/^\[dev-dependencies\]/,$d' Cargo.toml \
    && cargo build --release --bin tcp_server

FROM debian:bookworm-slim
COPY --from=build /src/SE050Sim/se050-sim/target/release/tcp_server /usr/local/bin/se050-sim
RUN mkdir -p /data && chown 65534:65534 /data
VOLUME /data
ENV SE050_SIM_STORE=/data/store.json
EXPOSE 8050
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/se050-sim"]
