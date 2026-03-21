# Multi-stage build for wfweb
# Supports linux/amd64 and linux/arm64

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git \
    qt5-qmake qtbase5-dev libqt5serialport5-dev \
    qtmultimedia5-dev libqt5websockets5-dev \
    libqt5gamepad5-dev libqt5printsupport5 \
    libopus-dev libeigen3-dev \
    portaudio19-dev librtaudio-dev \
    libhidapi-dev libudev-dev libpulse-dev \
    libqcustomplot-dev \
    openssl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN qmake wfweb.pro && make -j$(nproc)
RUN make install INSTALL_ROOT=/install

# Runtime stage
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl \
    libqt5core5t64 libqt5gui5t64 libqt5widgets5t64 \
    libqt5network5t64 libqt5serialport5 libqt5multimedia5 \
    libqt5websockets5 libqt5gamepad5 libqt5printsupport5t64 \
    libopus0 libportaudio2 libpulse0 libhidapi-libusb0 \
    libudev1 libqcustomplot2.1 librtaudio6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /install/usr/local /usr/local

ENV QT_QPA_PLATFORM=offscreen

EXPOSE 8080 8081

ENTRYPOINT ["wfweb"]
CMD []
