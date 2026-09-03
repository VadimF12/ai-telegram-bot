# 1. Используем официальный образ Ubuntu для сборки
FROM ubuntu:24.04 AS builder

# Отключаем интерактивные диалоги при установке пакетов
ENV DEBIAN_FRONTEND=noninteractive

# Устанавливаем компилятор C++, CMake, Ninja и все необходимые библиотеки
RUN apt-get update && apt-get install -y \
    g++ \
    cmake \
    ninja-build \
    git \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    nlohmann-json3-dev \
    libboost-system-dev \
    && rm -rf /var/lib/apt/lists/*

# Скачиваем и собираем библиотеку tgbot-cpp
RUN git clone https://github.com/reo7sp/tgbot-cpp.git /tmp/tgbot-cpp && \
    cd /tmp/tgbot-cpp && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF && \
    cmake --build build && \
    cmake --install build && \
    rm -rf /tmp/tgbot-cpp

# Копируем исходный код нашего проекта в контейнер
WORKDIR /app
COPY . .

# Собираем наш проект
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

# 2. Финальный легкий образ для запуска
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Устанавливаем только Runtime-зависимости (без компиляторов)
RUN apt-get update && apt-get install -y \
    libsqlite3-0 \
    libcurl4 \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Копируем скомпилированный бинарник из этапа сборки
COPY --from=builder /app/build/untitled5 /app/bot

# Создаем папку для базы данных SQLite, чтобы данные сохранялись
VOLUME ["/app/data"]

# Запускаем бота
CMD ["/app/bot"]