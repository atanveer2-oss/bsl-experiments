# Reproducible Linux build of the vendored SetBench fork.
#
# Works on x86_64 and aarch64: the portability patch dispatches on the target
# triple, so the same image definition builds natively on both. Use x86_64 for
# numbers that go in the paper (it is what the competing papers measured on);
# aarch64 is for developing and validating the harness locally on Apple silicon.
#
#   docker build -t bsl-setbench experiments/setbench
#   docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/build.sh
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++ make git python3 numactl \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
