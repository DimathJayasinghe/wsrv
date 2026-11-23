# The load generator as a container, so it talks to each contender over the Docker network
# (host-published ports go through Docker Desktop's proxy, which would distort every number).
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ libssl-dev \
    && rm -rf /var/lib/apt/lists/*
COPY wsbench.cpp /src/wsbench.cpp
RUN g++ -std=c++20 -O2 -pthread /src/wsbench.cpp -o /wsbench -lcrypto

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /wsbench /usr/local/bin/wsbench
ENTRYPOINT ["wsbench"]
