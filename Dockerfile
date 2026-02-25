FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN make clean && make

# Domyslne: 5000 klientow, 1500 w sklepie, 08:00-23:00, 100ms/min
CMD ["./kierownik"]
