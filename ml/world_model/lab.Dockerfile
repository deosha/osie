# Disposable lab image for the CPU world-model experiments: builds
# osie_wm_lab from this checkout and runs it unprivileged.
FROM osie-linux-dev AS build
COPY . /src
RUN cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release -DOSIE_BUILD_TESTS=OFF -DOSIE_BUILD_BENCHMARKS=OFF \
    && ninja -C /b osie_wm_lab osie_cpu_lab

FROM ubuntu:24.04
COPY --from=build /b/osie_wm_lab /b/osie_cpu_lab /usr/local/bin/
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/osie_wm_lab", "serve"]
