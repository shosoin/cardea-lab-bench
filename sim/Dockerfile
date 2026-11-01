# SE050Sim (wolfSSL, https://github.com/wolfSSL/simulators, GPL-3.0-or-later) TCP server for the lab:
# it speaks raw T=1-over-I2C frames on TCP port 8050. NOT a security device.
# Built from the repository and commit below (a fork with the features this lab uses).
FROM rust:1.85-bookworm AS build
ARG SIMULATORS_REPO=https://github.com/shosoin/simulators.git
ARG SIMULATORS_COMMIT=e2ac6ec6995f8b7bd4e9f1ec283d5ef441c7a354
WORKDIR /src
RUN git clone "$SIMULATORS_REPO" . && git checkout --quiet "$SIMULATORS_COMMIT"
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
