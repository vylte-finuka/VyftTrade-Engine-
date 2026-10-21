# Image du serveur Vyft Trade™ (vyft-trade.vyft-one.com)
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates libcurl4-openssl-dev xz-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target VyftTradeServer -j

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/VyftTradeServer /usr/local/bin/VyftTradeServer
# Positions persistantes : monter un volume sur /data
VOLUME /data
EXPOSE 8080
CMD ["VyftTradeServer", "8080", "/data/orders.json"]
