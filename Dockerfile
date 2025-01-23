FROM debian:12 AS base_env
RUN apt update -y &&\
    apt upgrade -y

FROM base_env AS build_env
RUN apt install libfuse3-dev build-essential pkg-config -y
WORKDIR build
COPY Makefile .
COPY src ./src/
RUN make

FROM base_env AS final_env
COPY --from=build_env /build/scriptfs /bin/scriptfs
RUN apt install fuse3 -y &&\
    chmod +x /bin/scriptfs
ENTRYPOINT ["/bin/bash"]
